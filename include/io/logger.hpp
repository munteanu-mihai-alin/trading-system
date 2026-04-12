#pragma once
#include <fstream>
#include <string>

namespace hft {

class Logger {
  std::ofstream out_;

 public:
  explicit Logger(const std::string& file) : out_(file) {
    out_ << "step,symbol,mode,pnl,total_pnl,trades\n";
  }

  void log(int step, const std::string& sym, const std::string& mode,
           double pnl, double total, int trades) {
    out_ << step << ',' << sym << ',' << mode << ',' << pnl << ',' << total
         << ',' << trades << '\n';
  }
};

}  // namespace hft
