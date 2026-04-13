#pragma once
#include <string>
#include <unordered_map>

namespace hft {

enum class OrderLifecycleStatus {
  New,
  Submitted,
  PartiallyFilled,
  Filled,
  Cancelled,
  Rejected,
  Unknown
};

struct OrderLifecycleState {
  int id = 0;
  std::string symbol;
  double requested_qty = 0.0;
  double filled_qty = 0.0;
  double remaining_qty = 0.0;
  double avg_fill_price = 0.0;
  OrderLifecycleStatus status = OrderLifecycleStatus::New;
};

class OrderLifecycleBook {
  std::unordered_map<int, OrderLifecycleState> states_;

 public:
  void on_submitted(int id, const std::string& symbol, double qty) {
    auto& s = states_[id];
    s.id = id;
    s.symbol = symbol;
    s.requested_qty = qty;
    s.remaining_qty = qty;
    s.status = OrderLifecycleStatus::Submitted;
  }

  void on_status(int id, const std::string& status, double filled,
                 double remaining, double avg_fill_price) {
    auto& s = states_[id];
    s.id = id;
    s.filled_qty = filled;
    s.remaining_qty = remaining;
    s.avg_fill_price = avg_fill_price;

    if (status == "Submitted" || status == "PreSubmitted") {
      s.status = OrderLifecycleStatus::Submitted;
    } else if (status == "Filled") {
      s.status = OrderLifecycleStatus::Filled;
    } else if (status == "Cancelled" || status == "ApiCancelled") {
      s.status = OrderLifecycleStatus::Cancelled;
    } else if (status == "Inactive") {
      s.status = OrderLifecycleStatus::Rejected;
    } else if (filled > 0.0 && remaining > 0.0) {
      s.status = OrderLifecycleStatus::PartiallyFilled;
    } else {
      s.status = OrderLifecycleStatus::Unknown;
    }
  }

  [[nodiscard]] bool has(int id) const {
    return states_.find(id) != states_.end();
  }

  [[nodiscard]] const OrderLifecycleState* get(int id) const {
    const auto it = states_.find(id);
    if (it == states_.end())
      return nullptr;
    return &it->second;
  }
};

}  // namespace hft
