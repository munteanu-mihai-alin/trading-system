#pragma once
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "broker/IBroker.hpp"
#include "config/AppConfig.hpp"

namespace hft {

class DatabentoBacktestBroker : public IBroker {
  struct ReplaySeries {
    std::string symbol;
    std::vector<L2Book> books;
    L2Book current;
  };

  bool connected_ = false;
  int current_step_ = 0;
  AppConfig cfg_;
  OrderLifecycleBook lifecycle_;
  std::unordered_map<int, ReplaySeries> replay_by_ticker_;
  std::unordered_map<int, OrderRequest> working_orders_;

  [[nodiscard]] std::filesystem::path cache_path_for_symbol(
      const std::string& symbol) const;
  [[nodiscard]] std::string downloader_command(const std::string& symbol,
                                               const std::filesystem::path& out,
                                               int depth) const;
  bool ensure_symbol_loaded(const MarketDepthRequest& req);
  bool download_symbol_if_missing(const std::string& symbol,
                                  const std::filesystem::path& out,
                                  int depth) const;
  [[nodiscard]] std::vector<L2Book> load_books_from_csv(
      const std::filesystem::path& path) const;
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
  void subscribe_market_depth(const MarketDepthRequest& req) override;
  void on_step(int t) override;

  [[nodiscard]] L2Book snapshot_book(int ticker_id) const override;
  [[nodiscard]] const OrderLifecycleBook* order_lifecycle() const override;
};

}  // namespace hft
