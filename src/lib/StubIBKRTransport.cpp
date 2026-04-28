// Stub transport used when HFT_ENABLE_IBKR=OFF.
//
// Rationale: keeps the non-IBKR build working (Linux CI default, the
// build-and-test job, the coverage job) and the IBKR-stub-style tests
// passing (they never expected real TWS traffic, just a plausible "broker
// is connected" answer). This file is selected by CMake based on the
// HFT_ENABLE_IBKR option. Phase 2 of the IBKR refactor will remove the
// option and delete this file.

#include "broker/IBKRCallbacks.hpp"
#include "broker/IBKRTransport.hpp"

namespace hft {

namespace {

class StubIBKRTransport : public IBKRTransport {
 public:
  bool connect(const std::string&, int, int) override {
    connected_ = true;
    return true;
  }

  void disconnect() override { connected_ = false; }

  [[nodiscard]] bool is_connected() const override { return connected_; }

  void place_limit_order(const OrderRequest&) override {}
  void cancel_order(int) override {}
  void subscribe_market_depth(const MarketDepthRequest&) override {}

  void pump_once() override {}

  void set_callbacks(IBKRCallbacks* cb) override { callbacks_ = cb; }

 private:
  bool connected_ = false;
  IBKRCallbacks* callbacks_ = nullptr;
};

}  // namespace

std::unique_ptr<IBKRTransport> make_default_ibkr_transport() {
  return std::make_unique<StubIBKRTransport>();
}

}  // namespace hft
