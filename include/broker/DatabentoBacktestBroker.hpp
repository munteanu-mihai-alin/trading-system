#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "broker/IBroker.hpp"
#include "broker/cache_filename.hpp"
#include "config/AppConfig.hpp"

namespace hft {

class DatabentoBacktestBroker : public IBroker {
  struct ReplaySeries {
    std::string symbol;
    std::vector<L2Book> books;
    // Parallel ts_event_ns per book row. Empty when the cache file is the
    // legacy (no ts_event) schema; in that case `on_step` falls back to
    // step-index advancement.
    std::vector<std::int64_t> ts_events;
    L2Book current;
    // Monotonically-advancing cursor into `books`. Used by `on_step` to walk
    // L2 rows that fall within the current engine wall-clock (taken from the
    // matching L1 series' ts_event). Not the same as the engine's logical
    // step counter, which over-advances dense L2 streams.
    std::size_t cursor = 0;
  };

  struct TopReplaySeries {
    std::string symbol;
    std::vector<TopOfBook> books;
    std::vector<std::int64_t> ts_events;  // parallel to books; 0 if legacy
    TopOfBook current;
    std::int64_t current_ts_event = 0;
  };

  bool connected_ = false;
  int current_step_ = 0;
  AppConfig cfg_;
  OrderLifecycleBook lifecycle_;
  std::unordered_map<int, TopReplaySeries> top_replay_by_ticker_;
  std::unordered_map<int, ReplaySeries> replay_by_ticker_;
  std::unordered_map<int, OrderRequest> working_orders_;

  // New dated-cache layout helpers (see include/broker/cache_filename.hpp).
  //
  // Path of the file the broker WOULD write if a download were necessary
  // right now, given the configured window. The filename encodes the
  // REQUESTED range (cfg_.databento_start / databento_end) - downloading
  // produces a file whose actual data fits within that range, possibly
  // slightly shorter at the boundaries (handled by cache_covers_window
  // tolerances on future lookups).
  [[nodiscard]] std::filesystem::path new_download_path_for_symbol(
      const std::filesystem::path& root, const std::string& symbol,
      cache::Kind kind, std::int64_t req_start_ns,
      std::int64_t req_end_ns) const;

 public:
  // Coverage plan for a single symbol: existing files to reuse + any
  // missing windows to download. The planner picks a minimal set of
  // existing files that, together with the gap downloads, cover
  // [req_start_ns, req_end_ns]. Public so unit tests can construct one
  // without standing up the broker.
  struct CoveragePlan {
    // Paths to existing cached files that contribute to coverage.
    // Sorted by start_ns at plan-build time so a downstream concat is
    // already in time order.
    std::vector<std::filesystem::path> reuse_paths;
    // (start_ns, end_ns) for each missing chunk. Each gap becomes a
    // separate download call. The downloader writes a new dated file
    // per gap; the engine reads existing + gap files together.
    std::vector<std::pair<std::int64_t, std::int64_t>> gap_ranges;
  };

  // Plan computation given a pre-collected candidate list. Pure
  // function: no I/O, no broker state. Public + static so unit tests
  // can exercise the planning logic without standing up the broker or
  // touching the filesystem.
  //
  // Each candidate is (path, start_ns, end_ns). Candidates may arrive
  // in any order; the planner sorts internally. Files that are fully
  // covered by an earlier-selected file are skipped (no duplicate
  // load). Interior gaps shorter than ~5 min are treated as no-gap
  // (small boundary fuzz, e.g. when two downloads bracket the same
  // off-hours minute).
  [[nodiscard]] static CoveragePlan plan_coverage_from_candidates(
      std::vector<std::tuple<std::filesystem::path, std::int64_t, std::int64_t>>
          candidates,
      std::int64_t req_start_ns, std::int64_t req_end_ns);

 private:
  // I/O wrapper: globs <root>/**/<SAFE_SYM>_*.<ext> for the symbol,
  // parses each filename via cache::parse_filename, then delegates to
  // plan_coverage_from_candidates.
  [[nodiscard]] CoveragePlan compute_coverage_plan(
      const std::filesystem::path& root, const std::string& safe_symbol,
      cache::Kind kind, std::int64_t req_start_ns,
      std::int64_t req_end_ns) const;

  // Downloader command builders. Each takes an EXPLICIT [start_ns,
  // end_ns] - the broker passes the gap range for a stitched download
  // or the full config window for a fresh download. Format is ISO-8601
  // extended (with colons), which the downloader scripts already
  // accept on the --start/--end flags.
  [[nodiscard]] std::string l1_downloader_command(
      const std::string& symbol, const std::filesystem::path& out,
      std::int64_t start_ns, std::int64_t end_ns) const;
  [[nodiscard]] std::string l2_downloader_command(
      const std::string& symbol, const std::filesystem::path& out, int depth,
      std::int64_t start_ns, std::int64_t end_ns) const;
  bool ensure_l1_symbol_loaded(const TopOfBookRequest& req);
  bool ensure_l2_symbol_loaded(const MarketDepthRequest& req);
  bool download_if_missing(const std::filesystem::path& out,
                           const std::string& command) const;
  // out_ts_events (optional) is populated with one ts_event_ns per returned
  // book row, parallel to the books vector. Used by `on_step` to time-pace
  // L2 advancement against the matching L1 ts_event for the same symbol.
  [[nodiscard]] std::vector<TopOfBook> load_top_books_from_csv(
      const std::filesystem::path& path,
      std::optional<std::int64_t> start_ns = std::nullopt,
      std::optional<std::int64_t> end_ns = std::nullopt,
      std::vector<std::int64_t>* out_ts_events = nullptr) const;
  [[nodiscard]] std::vector<L2Book> load_books_from_csv(
      const std::filesystem::path& path,
      std::optional<std::int64_t> start_ns = std::nullopt,
      std::optional<std::int64_t> end_ns = std::nullopt,
      std::vector<std::int64_t>* out_ts_events = nullptr) const;

  // Multi-file variants: load each file in order with the same window
  // filter, concatenate rows into a single time-ordered output. Callers
  // pass paths sorted by start_ns (the coverage planner does this).
  // Assumes minimal overlap between files (planner skips redundant
  // ones); any small overlap inside [start_ns, end_ns] produces dup
  // rows that the engine tolerates (last-row-wins on book state).
  [[nodiscard]] std::vector<TopOfBook> load_top_books_from_csvs(
      const std::vector<std::filesystem::path>& paths,
      std::optional<std::int64_t> start_ns, std::optional<std::int64_t> end_ns,
      std::vector<std::int64_t>* out_ts_events) const;
  [[nodiscard]] std::vector<L2Book> load_books_from_csvs(
      const std::vector<std::filesystem::path>& paths,
      std::optional<std::int64_t> start_ns, std::optional<std::int64_t> end_ns,
      std::vector<std::int64_t>* out_ts_events) const;
  [[nodiscard]] TopOfBook top_for_symbol(const std::string& symbol) const;
  [[nodiscard]] L2Book depth_for_symbol(const std::string& symbol) const;
  void fill_crossed_orders();

 public:
  explicit DatabentoBacktestBroker(AppConfig cfg);

  bool connect(const std::string& host, int port, int client_id) override;
  void disconnect() override;
  bool is_connected() const override;
  void place_limit_order(const OrderRequest& req) override;
  void cancel_order(int order_id) override;
  void start_event_loop() override;
  void stop_event_loop() override;
  void subscribe_top_of_book(const TopOfBookRequest& req) override;
  void subscribe_market_depth(const MarketDepthRequest& req) override;
  void on_step(int t) override;

  [[nodiscard]] TopOfBook snapshot_top_of_book(int ticker_id) const override;
  [[nodiscard]] L2Book snapshot_book(int ticker_id) const override;
  [[nodiscard]] const OrderLifecycleBook* order_lifecycle() const override;
  [[nodiscard]] int max_replay_steps() const override;

  // Decides whether a dated L1/L2 cache file covers the requested window
  // well enough to reuse. Returns true when the cache can be reused as-is
  // (caller's load step will filter rows by ts_event), false when the
  // broker should re-download.
  //
  // Both bounds are tolerant by design:
  //   - Start: 1 minute. The first L1/L2 event of a session always arrives
  //     a few hundred ms after market open; strict cached_start > req_start
  //     would force pointless re-downloads.
  //   - End:   24 hours. The cached file's last ts_event is the last actual
  //     event from the source - often seconds shy of the requested midnight
  //     cutoff (markets are closed). Strict cached_end < req_end was
  //     burning ~$50-100 of Databento per universe-run before this method
  //     existed.
  //
  // Either bound passed as std::nullopt is treated as "no constraint on
  // that side". cached_range = std::nullopt (legacy schema or empty file)
  // returns false (force re-download). Public + static so the test fixture
  // can exercise the decision without standing up the broker.
  [[nodiscard]] static bool cache_covers_window(
      const std::optional<std::pair<std::int64_t, std::int64_t>>& cached_range,
      const std::optional<std::int64_t>& req_start,
      const std::optional<std::int64_t>& req_end);
};

}  // namespace hft
