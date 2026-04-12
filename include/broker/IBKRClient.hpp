
#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "broker/ConnectionSupervisor.hpp"
#include "broker/IBroker.hpp"
#include "broker/OrderLifecycle.hpp"
#include "models/l2_book.hpp"

#ifdef HFT_ENABLE_IBKR
#include "Contract.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "EWrapper.h"
#include "Order.h"
#endif

namespace hft {

// Compile-gated IBKR adapter.
// - Without HFT_ENABLE_IBKR: safe stub that preserves buildability.
// - With HFT_ENABLE_IBKR: wraps the official IBKR C++ SDK.
class IBKRClient
#ifdef HFT_ENABLE_IBKR
    : public IBroker,
      public EWrapper
#else
    : public IBroker
#endif
{
  bool connected_ = false;
  std::unordered_map<int, std::chrono::high_resolution_clock::time_point>
      send_ts_;
  std::unordered_map<int, double> ack_latency_ms_cache_;
  std::unordered_map<int, L2Book> books_;
  OrderLifecycleBook lifecycle_;
  ConnectionSupervisor reconnect_;
  std::string host_;
  int port_ = 0;
  int client_id_ = 0;

  mutable std::mutex books_mutex_;
#ifdef HFT_ENABLE_IBKR
  EReaderOSSignal signal_{2000};
  EClientSocket client_{this, &signal_};
  EReader* reader_ = nullptr;
  std::atomic<bool> reader_running_{false};
  std::thread reader_thread_;
#endif

 public:
  IBKRClient() = default;
  ~IBKRClient() override;

  bool connect(const std::string& host, int port, int client_id) override;
  void disconnect() override;
  bool is_connected() const override;
  void place_limit_order(const OrderRequest& req) override;
  void cancel_order(int order_id) override;
  void start_event_loop() override;
  void stop_event_loop() override;
  void subscribe_market_depth(const MarketDepthRequest& req) override;
  void start_production_event_loop();
  void pump_once();
  bool reconnect_once();

  [[nodiscard]] double ack_latency_ms(int order_id) const;
  [[nodiscard]] L2Book snapshot_book(int ticker_id) const;
  [[nodiscard]] const OrderLifecycleBook& lifecycle() const {
    return lifecycle_;
  }

#ifdef HFT_ENABLE_IBKR
  // Core callbacks we actively use.
  void orderStatus(OrderId orderId, const std::string& status, Decimal filled,
                   Decimal remaining, double avgFillPrice, int permId,
                   int parentId, double lastFillPrice, int clientId,
                   const std::string& whyHeld, double mktCapPrice) override;

  void nextValidId(OrderId orderId) override {}
  void updateMktDepth(TickerId id, int position, int operation, int side,
                      double price, Decimal size) override;

  // Required no-op overrides to satisfy EWrapper.
  void tickPrice(TickerId, TickType, double, const TickAttrib&) override {}
  void tickSize(TickerId, TickType, Decimal) override {}
  void tickString(TickerId, TickType, const std::string&) override {}
  void tickGeneric(TickerId, TickType, double) override {}
  void tickEFP(TickerId, TickType, double, const std::string&, double, int,
               const std::string&, double, double) override {}
  void openOrder(OrderId, const Contract&, const Order&,
                 const OrderState&) override {}
  void openOrderEnd() override {}
  void winError(const std::string&, int) override {}
  void connectionClosed() override { connected_ = false; }
  void updateAccountValue(const std::string&, const std::string&,
                          const std::string&, const std::string&) override {}
  void updatePortfolio(const Contract&, Decimal, double, double, double, double,
                       double, const std::string&) override {}
  void updateAccountTime(const std::string&) override {}
  void accountDownloadEnd(const std::string&) override {}
  void contractDetails(int, const ContractDetails&) override {}
  void bondContractDetails(int, const ContractDetails&) override {}
  void contractDetailsEnd(int) override {}
  void execDetails(int, const Contract&, const Execution&) override {}
  void execDetailsEnd(int) override {}
  void updateMktDepth(TickerId, int, int, int, double, Decimal) override {}
  void updateMktDepthL2(TickerId, int, const std::string&, int, int, double,
                        Decimal, bool) override {}
  void updateNewsBulletin(int, int, const std::string&,
                          const std::string&) override {}
  void managedAccounts(const std::string&) override {}
  void receiveFA(faDataType, const std::string&) override {}
  void historicalData(TickerId, const Bar&) override {}
  void historicalDataEnd(int, const std::string&, const std::string&) override {
  }
  void scannerParameters(const std::string&) override {}
  void scannerData(int, int, const ContractDetails&, const std::string&,
                   const std::string&, const std::string&,
                   const std::string&) override {}
  void scannerDataEnd(int) override {}
  void realtimeBar(TickerId, long, double, double, double, double, Decimal,
                   Decimal, int) override {}
  void currentTime(long) override {}
  void fundamentalData(TickerId, const std::string&) override {}
  void deltaNeutralValidation(int, const DeltaNeutralContract&) override {}
  void tickSnapshotEnd(int) override {}
  void marketDataType(TickerId, int) override {}
  void commissionReport(const CommissionReport&) override {}
  void position(const std::string&, const Contract&, Decimal, double) override {
  }
  void positionEnd() override {}
  void accountSummary(int, const std::string&, const std::string&,
                      const std::string&, const std::string&) override {}
  void accountSummaryEnd(int) override {}
  void verifyMessageAPI(const std::string&) override {}
  void verifyCompleted(bool, const std::string&) override {}
  void verifyAndAuthMessageAPI(const std::string&,
                               const std::string&) override {}
  void verifyAndAuthCompleted(bool, const std::string&) override {}
  void displayGroupList(int, const std::string&) override {}
  void displayGroupUpdated(int, const std::string&) override {}
  void error(int, int, const std::string&, const std::string&) override {}
  void error(const std::exception&) override {}
  void error(const std::string&) override {}
  void connectAck() override {}
  void positionMulti(int, const std::string&, const std::string&,
                     const Contract&, Decimal, double) override {}
  void positionMultiEnd(int) override {}
  void accountUpdateMulti(int, const std::string&, const std::string&,
                          const std::string&, const std::string&,
                          const std::string&) override {}
  void accountUpdateMultiEnd(int) override {}
  void securityDefinitionOptionalParameter(int, const std::string&, int,
                                           const std::string&,
                                           const std::string&,
                                           const std::set<std::string>&,
                                           const std::set<double>&) override {}
  void securityDefinitionOptionalParameterEnd(int) override {}
  void softDollarTiers(int, const std::vector<SoftDollarTier>&) override {}
  void familyCodes(const std::vector<FamilyCode>&) override {}
  void symbolSamples(int, const std::vector<ContractDescription>&) override {}
  void mktDepthExchanges(const std::vector<DepthMktDataDescription>&) override {
  }
  void tickNews(int, time_t, const std::string&, const std::string&,
                const std::string&, const std::string&) override {}
  void smartComponents(int, const SmartComponentsMap&) override {}
  void tickReqParams(int, double, const std::string&, int) override {}
  void newsProviders(const std::vector<NewsProvider>&) override {}
  void newsArticle(int, int, const std::string&) override {}
  void historicalNews(int, const std::string&, const std::string&,
                      const std::string&, const std::string&) override {}
  void historicalNewsEnd(int, bool) override {}
  void headTimestamp(int, const std::string&) override {}
  void histogramData(int, const HistogramDataVector&) override {}
  void historicalDataUpdate(TickerId, const Bar&) override {}
  void rerouteMktDataReq(int, int, const std::string&) override {}
  void rerouteMktDepthReq(int, int, const std::string&) override {}
  void marketRule(int, const PriceIncrement[]) override {}
  void pnl(int, double, double, double) override {}
  void pnlSingle(int, Decimal, double, double, double, double) override {}
  void historicalTicks(int, const std::vector<HistoricalTick>&, bool) override {
  }
  void historicalTicksBidAsk(int, const std::vector<HistoricalTickBidAsk>&,
                             bool) override {}
  void historicalTicksLast(int, const std::vector<HistoricalTickLast>&,
                           bool) override {}
  void tickByTickAllLast(int, int, time_t, double, Decimal,
                         const TickAttribLast&, const std::string&,
                         const std::string&) override {}
  void tickByTickBidAsk(int, time_t, double, double, Decimal, Decimal,
                        const TickAttribBidAsk&) override {}
  void tickByTickMidPoint(int, time_t, double) override {}
  void orderBound(long long, int, int) override {}
  void completedOrder(const Contract&, const Order&,
                      const OrderState&) override {}
  void completedOrdersEnd() override {}
  void replaceFAEnd(int, const std::string&) override {}
  void wshMetaData(int, const std::string&) override {}
  void wshEventData(int, const std::string&) override {}
  void historicalSchedule(int, const std::string&, const std::string&,
                          const std::string&,
                          const std::vector<HistoricalSession>&) override {}
  void userInfo(int, const std::string&) override {}
#endif
};

}  // namespace hft
