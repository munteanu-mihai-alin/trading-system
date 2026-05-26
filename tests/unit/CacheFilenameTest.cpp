// Unit tests for hft::cache filename codec.
//
// These functions are pure; the test cares about format round-trip,
// rejection of malformed inputs, and the symbol-with-underscore edge case
// (BRK.B -> BRK_B via safe_symbol_filename).

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>

#include "broker/cache_filename.hpp"

namespace {

using hft::cache::format_date;
using hft::cache::format_filename;
using hft::cache::format_folder_name;
using hft::cache::format_iso8601_compact;
using hft::cache::Kind;
using hft::cache::parse_filename;
using hft::cache::parse_iso8601_compact;
using hft::cache::ParsedFilename;

// 2020-03-09T13:30:00Z -> 1583760600 sec -> 1583760600000000000 ns.
constexpr std::int64_t kCovidStartNs = 1583760600000000000LL;
// 2020-03-20T20:00:00Z -> 1584734400 sec -> 1584734400000000000 ns.
constexpr std::int64_t kCovidEndNs = 1584734400000000000LL;

TEST(CacheFilename, ParseIso8601CompactKnownValue) {
  const auto parsed = parse_iso8601_compact("20200309T133000Z");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, kCovidStartNs);
}

TEST(CacheFilename, FormatIso8601CompactKnownValue) {
  EXPECT_EQ(format_iso8601_compact(kCovidStartNs), "20200309T133000Z");
  EXPECT_EQ(format_iso8601_compact(kCovidEndNs), "20200320T200000Z");
}

TEST(CacheFilename, IsoCompactRoundTripsForManyTimestamps) {
  // A spread of historically interesting moments; checks DST-free UTC
  // handling and various month/day boundaries.
  for (
      const std::int64_t ts : {
          kCovidStartNs,
          kCovidEndNs,
          1722556200000000000LL,  // 2024-08-02T00:30:00Z (Yen unwind start-ish)
          1723219200000000000LL,  // 2024-08-09T16:00:00Z
          0LL,                    // 1970-01-01T00:00:00Z
          1735689600000000000LL,  // 2025-01-01T00:00:00Z (year boundary)
      }) {
    const auto formatted = format_iso8601_compact(ts);
    const auto reparsed = parse_iso8601_compact(formatted);
    ASSERT_TRUE(reparsed.has_value()) << "round-trip failed for " << ts;
    EXPECT_EQ(*reparsed, ts) << "round-trip mismatch via " << formatted;
  }
}

TEST(CacheFilename, FormatFilenameL2) {
  const auto name =
      format_filename("AAPL", kCovidStartNs, kCovidEndNs, Kind::L2);
  EXPECT_EQ(name, "AAPL_20200309T133000Z_20200320T200000Z.mbp10.csv");
}

TEST(CacheFilename, FormatFilenameL1) {
  const auto name =
      format_filename("NOK", kCovidStartNs, kCovidEndNs, Kind::L1);
  EXPECT_EQ(name, "NOK_20200309T133000Z_20200320T200000Z.mbp1.csv");
}

TEST(CacheFilename, ParseRoundTripL2) {
  const auto name =
      format_filename("AAPL", kCovidStartNs, kCovidEndNs, Kind::L2);
  const auto parsed = parse_filename(name);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->symbol, "AAPL");
  EXPECT_EQ(parsed->start_ns, kCovidStartNs);
  EXPECT_EQ(parsed->end_ns, kCovidEndNs);
  EXPECT_EQ(parsed->kind, Kind::L2);
}

TEST(CacheFilename, ParseRoundTripL1) {
  const auto name =
      format_filename("NOK", kCovidStartNs, kCovidEndNs, Kind::L1);
  const auto parsed = parse_filename(name);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->symbol, "NOK");
  EXPECT_EQ(parsed->kind, Kind::L1);
}

TEST(CacheFilename, SymbolWithUnderscoreStillParses) {
  // safe_symbol_filename converts non-alnum to '_' (e.g. BRK.B -> BRK_B).
  // The parser anchors at the right edge and treats everything before the
  // trailing _<iso>_<iso> as the symbol, so embedded underscores survive.
  const auto name =
      format_filename("BRK_B", kCovidStartNs, kCovidEndNs, Kind::L2);
  EXPECT_EQ(name, "BRK_B_20200309T133000Z_20200320T200000Z.mbp10.csv");
  const auto parsed = parse_filename(name);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->symbol, "BRK_B");
}

TEST(CacheFilename, RejectsLegacyFilename) {
  EXPECT_FALSE(parse_filename("AAPL.mbp10.csv").has_value());
  EXPECT_FALSE(parse_filename("AAPL.mbp1.csv").has_value());
}

TEST(CacheFilename, RejectsUnknownSuffix) {
  EXPECT_FALSE(
      parse_filename("AAPL_20200309T133000Z_20200320T200000Z.txt").has_value());
  EXPECT_FALSE(parse_filename("AAPL_20200309T133000Z_20200320T200000Z.mbp5.csv")
                   .has_value());
  EXPECT_FALSE(parse_filename("").has_value());
}

TEST(CacheFilename, RejectsMalformedTimestamps) {
  // Extended ISO (with dashes/colons) is rejected - we use the basic form
  // specifically because colons are unsafe on Windows.
  EXPECT_FALSE(
      parse_filename("AAPL_2020-03-09T13:30:00Z_2020-03-20T20:00:00Z.mbp10.csv")
          .has_value());
  // Missing Z.
  EXPECT_FALSE(parse_filename("AAPL_20200309T133000_20200320T200000.mbp10.csv")
                   .has_value());
  // Wrong-length timestamp.
  EXPECT_FALSE(
      parse_filename("AAPL_20200309_20200320T200000Z.mbp10.csv").has_value());
}

TEST(CacheFilename, RejectsInvertedRange) {
  // end_ns <= start_ns is a sanity-fail; format_filename would never produce
  // this, but a hand-crafted filename should still be rejected.
  EXPECT_FALSE(
      parse_filename("AAPL_20200320T200000Z_20200309T133000Z.mbp10.csv")
          .has_value());
  // Same instant on both sides.
  EXPECT_FALSE(
      parse_filename("AAPL_20200309T133000Z_20200309T133000Z.mbp10.csv")
          .has_value());
}

TEST(CacheFilename, RejectsEmptySymbol) {
  EXPECT_FALSE(parse_filename("_20200309T133000Z_20200320T200000Z.mbp10.csv")
                   .has_value());
}

TEST(CacheFilename, RejectsBadIsoCalendar) {
  EXPECT_FALSE(
      parse_iso8601_compact("20200230T133000Z").has_value());  // Feb 30
  EXPECT_FALSE(
      parse_iso8601_compact("20201301T133000Z").has_value());  // month 13
  EXPECT_FALSE(
      parse_iso8601_compact("20200309T253000Z").has_value());  // hour 25
}

TEST(CacheFilename, FormatDateKnownValue) {
  EXPECT_EQ(format_date(kCovidStartNs), "2020-03-09");
  EXPECT_EQ(format_date(kCovidEndNs), "2020-03-20");
}

TEST(CacheFilename, FormatFolderNameKnownValue) {
  EXPECT_EQ(format_folder_name(kCovidStartNs, kCovidEndNs),
            "2020-03-09_2020-03-20");
}

}  // namespace
