#include "models/symbol_universe.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hft {

namespace {

std::string trim(const std::string& s) {
  const auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return {};
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

// Per-symbol IBKR primaryExchange override map. Built once at static
// initialisation time. Currently EMPTY because the symbol-contract probe
// (scripts/ibkr_symbol_contract_probe.py) hasn't been run against a live
// IB Gateway yet - that's the second half of audit item #9. Once the
// probe report identifies which symbols return ambiguous or wrong
// contracts under SMART alone, add them here as
//   {"PSTG", "NYSE"}, {"NIO", "NYSE"}, ...
// using the listing exchange reported by reqContractDetails.
//
// PSTG specifically failed the L1 historical backfill with both
// `--primary-exchange NASDAQ` and `--primary-exchange NYSE`; we left it
// out of this map until the probe confirms the correct value (it may
// need a different secType or a non-SMART exchange code).
const std::unordered_map<std::string, std::string>&
primary_exchange_override_table() {
  static const std::unordered_map<std::string, std::string> kTable = {
      // Intentionally empty until ibkr_symbol_contract_probe.py runs.
  };
  return kTable;
}

}  // namespace

std::string primary_exchange_for(const std::string& symbol) {
  const auto& table = primary_exchange_override_table();
  const auto it = table.find(symbol);
  if (it == table.end()) {
    return {};
  }
  return it->second;
}

std::vector<std::pair<std::string, std::string>> load_symbol_universe_from_file(
    const std::string& path) {
  std::vector<std::pair<std::string, std::string>> out;
  std::ifstream in(path);
  if (!in.is_open()) {
    // Caller decides what to do (typically fall back to kSymbolCompanyList).
    return out;
  }
  std::string line;
  while (std::getline(in, line)) {
    auto t = trim(line);
    if (t.empty() || t[0] == '#') {
      continue;
    }
    const auto comma = t.find(',');
    std::string symbol;
    std::string company;
    if (comma == std::string::npos) {
      symbol = trim(t);
    } else {
      symbol = trim(t.substr(0, comma));
      company = trim(t.substr(comma + 1));
    }
    if (symbol.empty())
      continue;
    out.emplace_back(std::move(symbol), std::move(company));
  }
  return out;
}

}  // namespace hft
