#pragma once

// Pure functions for the dated-cache filename + folder convention.
//
// Layout:
//   data/l1/<startDate>_<endDate>/<SAFE_SYMBOL>_<startISO>_<endISO>.mbp1.csv
//   data/l2/<startDate>_<endDate>/<SAFE_SYMBOL>_<startISO>_<endISO>.mbp10.csv
//
// startDate / endDate are extended-ISO (YYYY-MM-DD) for human readability.
// startISO / endISO are basic-ISO 8601 (YYYYMMDDTHHMMSSZ, 16 chars). The
// basic form is intentional: colons are reserved on Windows (NTFS treats
// `:` as the alternate-data-stream separator), so any colon in a filename
// fails most Win32 file APIs. Folders use the extended date form because
// dashes are safe everywhere.
//
// The filename's time range is the ACTUAL data span (first/last ts_event
// observed), not the original requested window. That lets a future run
// answer "do I have a file that covers [Rs, Re]?" by scanning filenames
// only, without opening any file. The downloader is responsible for
// renaming its in-progress file to the final name once the actual span is
// known.

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace hft::cache {

enum class Kind {
  L1,  // .mbp1.csv
  L2,  // .mbp10.csv
};

struct ParsedFilename {
  std::string symbol;     // safe-form (alnum + underscore only)
  std::int64_t start_ns;  // inclusive
  std::int64_t end_ns;    // inclusive; > start_ns
  Kind kind;
};

// "20200309T133000Z" -> 1583760600000000000. Strict 16-char basic ISO 8601
// (no separators inside the timestamp). Returns nullopt for any deviation
// from the expected shape.
[[nodiscard]] inline std::optional<std::int64_t> parse_iso8601_compact(
    std::string_view iso) {
  if (iso.size() != 16)
    return std::nullopt;
  if (iso[8] != 'T' || iso[15] != 'Z')
    return std::nullopt;

  auto take = [&](std::size_t off, std::size_t n) -> int {
    int v = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const char c = iso[off + i];
      if (c < '0' || c > '9')
        return -1;
      v = v * 10 + (c - '0');
    }
    return v;
  };

  const int year = take(0, 4);
  const int mon = take(4, 2);
  const int day = take(6, 2);
  const int hour = take(9, 2);
  const int minute = take(11, 2);
  const int second = take(13, 2);
  if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > 31 || hour < 0 ||
      hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
    return std::nullopt;
  }

  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = mon - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
#if defined(_WIN32)
  const std::time_t t = _mkgmtime(&tm);
#else
  const std::time_t t = timegm(&tm);
#endif
  if (t == static_cast<std::time_t>(-1))
    return std::nullopt;
  return static_cast<std::int64_t>(t) * 1'000'000'000LL;
}

// 1583760600000000000 -> "20200309T133000Z". Truncates sub-second precision.
// Buffer is oversized vs the 17-byte output so GCC -Wformat-truncation does
// not flag the theoretical worst case (it can't prove tm.tm_year + 1900
// fits in 4 digits; runtime gmtime always produces a 4-digit year for the
// time_t range we use).
[[nodiscard]] inline std::string format_iso8601_compact(std::int64_t ts_ns) {
  const std::time_t t = static_cast<std::time_t>(ts_ns / 1'000'000'000LL);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d%02d%02dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec);
  return std::string(buf);
}

// 1583760600000000000 -> "2020-03-09". UTC date portion only.
[[nodiscard]] inline std::string format_date(std::int64_t ts_ns) {
  const std::time_t t = static_cast<std::time_t>(ts_ns / 1'000'000'000LL);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday);
  return std::string(buf);
}

// "2020-03-09_2020-03-20" - used as the parent directory name. Dashes inside
// each date stay (filesystem-safe) and an underscore separates the two
// dates so split('_') is unambiguous.
[[nodiscard]] inline std::string format_folder_name(std::int64_t start_ns,
                                                    std::int64_t end_ns) {
  return format_date(start_ns) + "_" + format_date(end_ns);
}

// "AAPL_20200309T133000Z_20200320T200000Z.mbp10.csv" - the leaf filename
// only. Caller appends to a parent directory.
[[nodiscard]] inline std::string format_filename(std::string_view symbol,
                                                 std::int64_t start_ns,
                                                 std::int64_t end_ns,
                                                 Kind kind) {
  std::string out;
  out.reserve(symbol.size() + 50);
  out.append(symbol);
  out.push_back('_');
  out.append(format_iso8601_compact(start_ns));
  out.push_back('_');
  out.append(format_iso8601_compact(end_ns));
  out.append(kind == Kind::L1 ? ".mbp1.csv" : ".mbp10.csv");
  return out;
}

// Parse the leaf filename produced by format_filename. The longer ".mbp10.csv"
// suffix is checked first to avoid a false match against ".mbp1.csv".
//
// Symbol may legitimately contain underscores (safe_symbol_filename converts
// non-alnum to '_', so e.g. BRK.B -> BRK_B). The two ISO timestamps have
// fixed length (16 each) so we anchor at the right and treat everything
// before the trailing _<iso>_<iso> as the symbol.
[[nodiscard]] inline std::optional<ParsedFilename> parse_filename(
    std::string_view filename) {
  static constexpr std::string_view kL1Suffix = ".mbp1.csv";
  static constexpr std::string_view kL2Suffix = ".mbp10.csv";

  Kind kind;
  std::string_view base;
  if (filename.size() > kL2Suffix.size() &&
      filename.substr(filename.size() - kL2Suffix.size()) == kL2Suffix) {
    kind = Kind::L2;
    base = filename.substr(0, filename.size() - kL2Suffix.size());
  } else if (filename.size() > kL1Suffix.size() &&
             filename.substr(filename.size() - kL1Suffix.size()) == kL1Suffix) {
    kind = Kind::L1;
    base = filename.substr(0, filename.size() - kL1Suffix.size());
  } else {
    return std::nullopt;
  }

  constexpr std::size_t kIsoLen = 16;
  // _<iso>_<iso> at the right edge = 1+16+1+16 = 34 chars. Need at least
  // 1 char left for the symbol.
  constexpr std::size_t kTimeSuffixLen = 1 + kIsoLen + 1 + kIsoLen;
  if (base.size() < kTimeSuffixLen + 1)
    return std::nullopt;

  const std::size_t end_at = base.size() - kIsoLen;
  if (base[end_at - 1] != '_')
    return std::nullopt;
  const std::string_view end_str = base.substr(end_at);

  const std::size_t start_at = end_at - 1 - kIsoLen;
  if (base[start_at - 1] != '_')
    return std::nullopt;
  const std::string_view start_str = base.substr(start_at, kIsoLen);

  const std::string_view symbol = base.substr(0, start_at - 1);
  if (symbol.empty())
    return std::nullopt;

  const auto start_ns = parse_iso8601_compact(start_str);
  const auto end_ns = parse_iso8601_compact(end_str);
  if (!start_ns || !end_ns)
    return std::nullopt;
  if (*end_ns <= *start_ns)
    return std::nullopt;

  ParsedFilename out;
  out.symbol = std::string(symbol);
  out.start_ns = *start_ns;
  out.end_ns = *end_ns;
  out.kind = kind;
  return out;
}

}  // namespace hft::cache
