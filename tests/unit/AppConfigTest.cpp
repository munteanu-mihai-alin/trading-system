// Unit tests for hft::AppConfig and AppConfig::load_from_file.
//
// load_from_file parses a tiny key=value config (the same one main.cpp reads
// from `config.ini`). We exercise: defaults, all known keys, mode mapping,
// port() helper, comment/section/blank lines, malformed lines, invalid
// integer values, and the missing-file fallback.

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "config/AppConfig.hpp"

namespace {

using hft::AppConfig;
using hft::BrokerMode;

// Write a temp file containing `body`, return its path. Caller is responsible
// for std::remove() after use - tests do that via RAII below.
class TempIni {
 public:
  explicit TempIni(const std::string& body) {
    path_ = "test_appconfig_" +
            std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".ini";
    std::ofstream out(path_);
    out << body;
  }
  ~TempIni() { std::remove(path_.c_str()); }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

TEST(AppConfig, DefaultsAreSane) {
  AppConfig cfg;
  EXPECT_EQ(cfg.mode, BrokerMode::Paper);
  EXPECT_EQ(cfg.host, "127.0.0.1");
  EXPECT_EQ(cfg.paper_port, 7497);
  EXPECT_EQ(cfg.live_port, 7496);
  EXPECT_EQ(cfg.client_id, 1);
  EXPECT_EQ(cfg.top_k, 3);
  EXPECT_EQ(cfg.steps, 500);
}

TEST(AppConfig, PortPaperVsLive) {
  AppConfig cfg;
  cfg.mode = BrokerMode::Paper;
  EXPECT_EQ(cfg.port(), cfg.paper_port);
  cfg.mode = BrokerMode::Sim;
  EXPECT_EQ(cfg.port(), cfg.paper_port);  // sim falls back to paper port
  cfg.mode = BrokerMode::Live;
  EXPECT_EQ(cfg.port(), cfg.live_port);
}

TEST(AppConfig, MissingFileReturnsDefaults) {
  // Path that almost certainly does not exist.
  const auto cfg = AppConfig::load_from_file("nonexistent_path_xyzzy.ini");
  EXPECT_EQ(cfg.mode, BrokerMode::Paper);
  EXPECT_EQ(cfg.host, "127.0.0.1");
  EXPECT_EQ(cfg.paper_port, 7497);
}

TEST(AppConfig, ParsesAllKnownKeys) {
  TempIni f(
      "mode=live\n"
      "host=10.0.0.5\n"
      "paper_port=1111\n"
      "live_port=2222\n"
      "client_id=42\n"
      "top_k=9\n"
      "steps=123\n");
  const auto cfg = AppConfig::load_from_file(f.path());
  EXPECT_EQ(cfg.mode, BrokerMode::Live);
  EXPECT_EQ(cfg.host, "10.0.0.5");
  EXPECT_EQ(cfg.paper_port, 1111);
  EXPECT_EQ(cfg.live_port, 2222);
  EXPECT_EQ(cfg.client_id, 42);
  EXPECT_EQ(cfg.top_k, 9);
  EXPECT_EQ(cfg.steps, 123);
}

TEST(AppConfig, ModeMapping) {
  {
    TempIni f("mode=paper\n");
    EXPECT_EQ(AppConfig::load_from_file(f.path()).mode, BrokerMode::Paper);
  }
  {
    TempIni f("mode=live\n");
    EXPECT_EQ(AppConfig::load_from_file(f.path()).mode, BrokerMode::Live);
  }
  {
    TempIni f("mode=sim\n");
    EXPECT_EQ(AppConfig::load_from_file(f.path()).mode, BrokerMode::Sim);
  }
  {
    TempIni f("mode=garbage\n");
    // Unknown mode falls back to Paper.
    EXPECT_EQ(AppConfig::load_from_file(f.path()).mode, BrokerMode::Paper);
  }
}

TEST(AppConfig, IgnoresCommentsAndSectionsAndBlanks) {
  TempIni f(
      "# this is a comment\n"
      "\n"
      "[section]\n"
      "host=1.2.3.4\n"
      "# trailing comment\n"
      "\n");
  const auto cfg = AppConfig::load_from_file(f.path());
  EXPECT_EQ(cfg.host, "1.2.3.4");
}

TEST(AppConfig, IgnoresLinesWithoutEquals) {
  TempIni f("not_a_kv_line\nhost=2.2.2.2\nanotherone\n");
  const auto cfg = AppConfig::load_from_file(f.path());
  EXPECT_EQ(cfg.host, "2.2.2.2");
}

TEST(AppConfig, TrimsWhitespaceAroundKeyAndValue) {
  TempIni f("   host  =   3.3.3.3  \n  client_id =  77\n");
  const auto cfg = AppConfig::load_from_file(f.path());
  EXPECT_EQ(cfg.host, "3.3.3.3");
  EXPECT_EQ(cfg.client_id, 77);
}

TEST(AppConfig, InvalidIntegerDoesNotCrash) {
  TempIni f("paper_port=not_a_number\nclient_id=12\n");
  const auto cfg = AppConfig::load_from_file(f.path());
  // Bad value is rejected silently; default kept. Other valid keys still apply.
  EXPECT_EQ(cfg.paper_port, 7497);
  EXPECT_EQ(cfg.client_id, 12);
}

TEST(AppConfig, UnknownKeyIgnored) {
  TempIni f("bogus_key=42\nhost=4.4.4.4\n");
  const auto cfg = AppConfig::load_from_file(f.path());
  EXPECT_EQ(cfg.host, "4.4.4.4");
}

}  // namespace
