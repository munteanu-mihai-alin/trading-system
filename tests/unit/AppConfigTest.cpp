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
  EXPECT_EQ(cfg.universe_size, 30);
  EXPECT_EQ(cfg.top_k, 3);
  EXPECT_EQ(cfg.steps, 500);
  EXPECT_TRUE(cfg.order_enabled);
  EXPECT_DOUBLE_EQ(cfg.order_qty, 10.0);
  EXPECT_DOUBLE_EQ(cfg.max_order_qty, 10.0);
  EXPECT_DOUBLE_EQ(cfg.max_notional_per_order, 0.0);
  EXPECT_EQ(cfg.max_open_symbols, 3);
  EXPECT_EQ(cfg.max_orders_per_run, 0);
  EXPECT_EQ(cfg.max_orders_per_symbol, 0);
  EXPECT_DOUBLE_EQ(cfg.target_profit_pct, 0.008);
  EXPECT_DOUBLE_EQ(cfg.min_sell_execution_score, 0.0);
  EXPECT_DOUBLE_EQ(cfg.commission_per_share, 0.005);
  EXPECT_DOUBLE_EQ(cfg.half_spread_cost, 0.0005);
  EXPECT_DOUBLE_EQ(cfg.impact_coefficient, 0.1);
  EXPECT_DOUBLE_EQ(cfg.assumed_daily_volume, 1'000'000.0);
  EXPECT_DOUBLE_EQ(cfg.daily_energy_kwh, 0.0);
  EXPECT_DOUBLE_EQ(cfg.energy_cost_per_kwh, 0.0);
  EXPECT_DOUBLE_EQ(cfg.daily_inflation_cost, 0.0);
  EXPECT_DOUBLE_EQ(cfg.expected_daily_shares, 1.0);
  EXPECT_EQ(cfg.databento_cache_dir, "data/databento");
  EXPECT_EQ(cfg.databento_python, "python");
  EXPECT_EQ(cfg.databento_l1_download_script,
            "scripts/databento_download_mbp1.py");
  EXPECT_EQ(cfg.databento_l2_download_script,
            "scripts/databento_download_l2.py");
  EXPECT_EQ(cfg.databento_l1_dataset, "EQUS.MINI");
  EXPECT_EQ(cfg.databento_l2_dataset, "XNAS.ITCH");
  EXPECT_EQ(cfg.databento_l1_schema, "mbp-1");
  EXPECT_EQ(cfg.databento_l2_schema, "mbp-10");
  EXPECT_TRUE(cfg.databento_start.empty());
  EXPECT_TRUE(cfg.databento_end.empty());
}

TEST(AppConfig, PortPaperVsLive) {
  AppConfig cfg;
  cfg.mode = BrokerMode::Paper;
  EXPECT_EQ(cfg.port(), cfg.paper_port);
  cfg.mode = BrokerMode::Sim;
  EXPECT_EQ(cfg.port(), cfg.paper_port);  // sim falls back to paper port
  cfg.mode = BrokerMode::IBKRPaper;
  EXPECT_EQ(cfg.port(), cfg.paper_port);
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
      "universe_size=17\n"
      "top_k=9\n"
      "steps=123\n"
      "order_enabled=false\n"
      "order_qty=1.5\n"
      "max_order_qty=2\n"
      "max_notional_per_order=500.25\n"
      "max_open_symbols=4\n"
      "max_orders_per_run=7\n"
      "max_orders_per_symbol=2\n"
      "target_profit_pct=0.012\n"
      "min_sell_execution_score=0.003\n"
      "commission_per_share=0.01\n"
      "half_spread_cost=0.0007\n"
      "impact_coefficient=0.2\n"
      "assumed_daily_volume=123456\n"
      "daily_energy_kwh=4.5\n"
      "energy_cost_per_kwh=0.31\n"
      "daily_inflation_cost=1.25\n"
      "expected_daily_shares=250\n"
      "databento_cache_dir=tmp/db\n"
      "databento_python=python3\n"
      "databento_l1_download_script=scripts/fetch_l1.py\n"
      "databento_l2_download_script=scripts/fetch_l2.py\n"
      "databento_l1_dataset=EQUS.MINI\n"
      "databento_l2_dataset=XNYS.PILLAR\n"
      "databento_l1_schema=mbp-1\n"
      "databento_l2_schema=mbp-10\n"
      "databento_start=2025-01-02T14:30:00Z\n"
      "databento_end=2025-01-02T14:35:00Z\n");
  const auto cfg = AppConfig::load_from_file(f.path());
  EXPECT_EQ(cfg.mode, BrokerMode::Live);
  EXPECT_EQ(cfg.host, "10.0.0.5");
  EXPECT_EQ(cfg.paper_port, 1111);
  EXPECT_EQ(cfg.live_port, 2222);
  EXPECT_EQ(cfg.client_id, 42);
  EXPECT_EQ(cfg.universe_size, 17);
  EXPECT_EQ(cfg.top_k, 9);
  EXPECT_EQ(cfg.steps, 123);
  EXPECT_FALSE(cfg.order_enabled);
  EXPECT_DOUBLE_EQ(cfg.order_qty, 1.5);
  EXPECT_DOUBLE_EQ(cfg.max_order_qty, 2.0);
  EXPECT_DOUBLE_EQ(cfg.max_notional_per_order, 500.25);
  EXPECT_EQ(cfg.max_open_symbols, 4);
  EXPECT_EQ(cfg.max_orders_per_run, 7);
  EXPECT_EQ(cfg.max_orders_per_symbol, 2);
  EXPECT_DOUBLE_EQ(cfg.target_profit_pct, 0.012);
  EXPECT_DOUBLE_EQ(cfg.min_sell_execution_score, 0.003);
  EXPECT_DOUBLE_EQ(cfg.commission_per_share, 0.01);
  EXPECT_DOUBLE_EQ(cfg.half_spread_cost, 0.0007);
  EXPECT_DOUBLE_EQ(cfg.impact_coefficient, 0.2);
  EXPECT_DOUBLE_EQ(cfg.assumed_daily_volume, 123456.0);
  EXPECT_DOUBLE_EQ(cfg.daily_energy_kwh, 4.5);
  EXPECT_DOUBLE_EQ(cfg.energy_cost_per_kwh, 0.31);
  EXPECT_DOUBLE_EQ(cfg.daily_inflation_cost, 1.25);
  EXPECT_DOUBLE_EQ(cfg.expected_daily_shares, 250.0);
  EXPECT_EQ(cfg.databento_cache_dir, "tmp/db");
  EXPECT_EQ(cfg.databento_python, "python3");
  EXPECT_EQ(cfg.databento_l1_download_script, "scripts/fetch_l1.py");
  EXPECT_EQ(cfg.databento_l2_download_script, "scripts/fetch_l2.py");
  EXPECT_EQ(cfg.databento_l1_dataset, "EQUS.MINI");
  EXPECT_EQ(cfg.databento_l2_dataset, "XNYS.PILLAR");
  EXPECT_EQ(cfg.databento_l1_schema, "mbp-1");
  EXPECT_EQ(cfg.databento_l2_schema, "mbp-10");
  EXPECT_EQ(cfg.databento_start, "2025-01-02T14:30:00Z");
  EXPECT_EQ(cfg.databento_end, "2025-01-02T14:35:00Z");
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
    TempIni f("mode=ibkr_paper\n");
    EXPECT_EQ(AppConfig::load_from_file(f.path()).mode, BrokerMode::IBKRPaper);
  }
  {
    TempIni f("mode=paper_ibkr\n");
    EXPECT_EQ(AppConfig::load_from_file(f.path()).mode, BrokerMode::IBKRPaper);
  }
  {
    TempIni f("mode=sim\n");
    EXPECT_EQ(AppConfig::load_from_file(f.path()).mode, BrokerMode::Sim);
  }
  {
    TempIni f("mode=databento_backtest\n");
    EXPECT_EQ(AppConfig::load_from_file(f.path()).mode,
              BrokerMode::DatabentoBacktest);
  }
  {
    TempIni f("mode=backtest\n");
    EXPECT_EQ(AppConfig::load_from_file(f.path()).mode,
              BrokerMode::DatabentoBacktest);
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
