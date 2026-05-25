#include "models/symbol_universe.hpp"

#include <fstream>
#include <sstream>
#include <string>
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

}  // namespace

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
