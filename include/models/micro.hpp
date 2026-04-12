#pragma once
#include <cmath>

namespace hft {

inline double microprice(double bid, double ask, double bid_vol,
                         double ask_vol) {
  const double denom = bid_vol + ask_vol + 1e-12;
  return (ask * bid_vol + bid * ask_vol) / denom;
}

inline double imbalance(double bid_vol, double ask_vol) {
  return (bid_vol - ask_vol) / (bid_vol + ask_vol + 1e-12);
}

}  // namespace hft
