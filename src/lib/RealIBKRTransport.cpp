// Real TWS-API-backed implementation of IBKRTransport.
//
// EClientSocket ownership, EReader signaling, and EWrapper callback overrides
// (most are no-ops; the few non-trivial ones translate Decimal -> double and
// forward to IBKRCallbacks) all live here, behind the IBKRTransport interface.
// This is the single transport implementation linked into hft_lib.

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "broker/IBKRCallbacks.hpp"
#include "broker/IBKRTransport.hpp"

#include "CommissionAndFeesReport.h"
#include "Contract.h"
#include "Decimal.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "EWrapper.h"
#include "Order.h"
#include "OrderCancel.h"

namespace hft {

namespace {

class RealIBKRTransport : public IBKRTransport, public EWrapper {
 public:
  RealIBKRTransport() = default;

  ~RealIBKRTransport() override {
    if (reader_ != nullptr) {
      reader_->stop();
      delete reader_;
      reader_ = nullptr;
    }
    if (client_.isConnected()) {
      client_.eDisconnect();
    }
  }

  // ---- IBKRTransport ----

  bool connect(const std::string& host, int port, int client_id) override {
    if (client_.eConnect(host.c_str(), port, client_id, false)) {
      connected_ = true;
      reader_ = new EReader(&client_, &signal_);
      reader_->start();
      return true;
    }
    connected_ = false;
    return false;
  }

  void disconnect() override {
    if (reader_ != nullptr) {
      reader_->stop();
      delete reader_;
      reader_ = nullptr;
    }
    if (client_.isConnected()) {
      client_.eDisconnect();
    }
    connected_ = false;
  }

  [[nodiscard]] bool is_connected() const override {
    return connected_ && client_.isConnected();
  }

  void place_limit_order(const OrderRequest& req) override {
    Contract contract;
    contract.symbol = req.symbol;
    contract.secType = "STK";
    contract.exchange = "SMART";
    contract.currency = "USD";

    Order order;
    order.orderId = req.id;
    order.action = req.is_buy ? "BUY" : "SELL";
    order.orderType = "LMT";
    order.totalQuantity = DecimalFunctions::doubleToDecimal(req.qty);
    order.lmtPrice = req.limit;
    order.transmit = req.transmit;

    client_.placeOrder(req.id, contract, order);
  }

  void cancel_order(int order_id) override {
    OrderCancel cancel;
    client_.cancelOrder(order_id, cancel);
  }

  void subscribe_top_of_book(const TopOfBookRequest& req) override {
    Contract contract;
    contract.symbol = req.symbol;
    contract.secType = "STK";
    contract.exchange = "SMART";
    contract.currency = "USD";
    client_.reqMktData(req.ticker_id, contract, "", false, false,
                       TagValueListSPtr());
  }

  void subscribe_market_depth(const MarketDepthRequest& req) override {
    Contract contract;
    contract.symbol = req.symbol;
    contract.secType = "STK";
    contract.exchange = "SMART";
    contract.currency = "USD";
    client_.reqMktDepth(req.ticker_id, contract, req.depth, false,
                        TagValueListSPtr());
  }

  void pump_once() override {
    if (!connected_)
      return;
    signal_.waitForSignal();
    if (reader_ != nullptr) {
      reader_->processMsgs();
    }
  }

  void set_callbacks(IBKRCallbacks* cb) override { callbacks_ = cb; }

  // ---- EWrapper: the few overrides we care about ----

  void orderStatus(OrderId orderId, const std::string& status, Decimal filled,
                   Decimal remaining, double avgFillPrice, long long /*permId*/,
                   int /*parentId*/, double /*lastFillPrice*/, int /*clientId*/,
                   const std::string& /*whyHeld*/,
                   double /*mktCapPrice*/) override {
    if (callbacks_ == nullptr)
      return;
    callbacks_->on_order_status(static_cast<int>(orderId), status,
                                DecimalFunctions::decimalToDouble(filled),
                                DecimalFunctions::decimalToDouble(remaining),
                                avgFillPrice);
  }

  void updateMktDepth(TickerId id, int position, int operation, int side,
                      double price, Decimal size) override {
    if (callbacks_ == nullptr)
      return;
    callbacks_->on_market_depth_update(static_cast<int>(id), position,
                                       operation, side, price,
                                       DecimalFunctions::decimalToDouble(size));
  }

  void connectionClosed() override {
    connected_ = false;
    if (callbacks_ != nullptr) {
      callbacks_->on_connection_closed();
    }
  }

  // ---- EWrapper: everything else is intentionally a no-op. ----
  // These overrides exist solely to satisfy the abstract base class. The
  // bodies match the previous stubs in IBKRClient.hpp 1:1.
  void nextValidId(OrderId orderId) override {
    if (callbacks_ != nullptr) {
      callbacks_->on_next_valid_id(static_cast<int>(orderId));
    }
  }
  void tickPrice(TickerId id, TickType field, double price,
                 const TickAttrib&) override {
    if (callbacks_ == nullptr)
      return;
    if (field == BID || field == DELAYED_BID) {
      callbacks_->on_top_of_book_price(static_cast<int>(id), true, price);
    } else if (field == ASK || field == DELAYED_ASK) {
      callbacks_->on_top_of_book_price(static_cast<int>(id), false, price);
    }
  }
  void tickSize(TickerId id, TickType field, Decimal size) override {
    if (callbacks_ == nullptr)
      return;
    const double value = DecimalFunctions::decimalToDouble(size);
    if (field == BID_SIZE || field == DELAYED_BID_SIZE) {
      callbacks_->on_top_of_book_size(static_cast<int>(id), true, value);
    } else if (field == ASK_SIZE || field == DELAYED_ASK_SIZE) {
      callbacks_->on_top_of_book_size(static_cast<int>(id), false, value);
    }
  }
  void tickOptionComputation(TickerId, TickType, int, double, double, double,
                             double, double, double, double, double) override {}
  void tickString(TickerId, TickType, const std::string&) override {}
  void tickGeneric(TickerId, TickType, double) override {}
  void tickEFP(TickerId, TickType, double, const std::string&, double, int,
               const std::string&, double, double) override {}
  void openOrder(OrderId, const Contract&, const Order&,
                 const OrderState&) override {}
  void openOrderEnd() override {}
  void winError(const std::string&, int) override {}
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
  void error(int id, time_t, int code, const std::string& message,
             const std::string& advancedOrderRejectJson) override {
    if (callbacks_ != nullptr) {
      callbacks_->on_error(
          IBKRError{id, code, message, advancedOrderRejectJson});
    }
  }
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
  void commissionAndFeesReport(const CommissionAndFeesReport&) override {}
  void position(const std::string&, const Contract&, Decimal, double) override {
  }
  void positionEnd() override {}
  void accountSummary(int, const std::string&, const std::string&,
                      const std::string&, const std::string&) override {}
  void accountSummaryEnd(int) override {}
  void verifyMessageAPI(const std::string&) override {}
  void verifyCompleted(bool, const std::string&) override {}
  void displayGroupList(int, const std::string&) override {}
  void displayGroupUpdated(int, const std::string&) override {}
  void verifyAndAuthMessageAPI(const std::string&,
                               const std::string&) override {}
  void verifyAndAuthCompleted(bool, const std::string&) override {}
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
  void marketRule(int, const std::vector<PriceIncrement>&) override {}
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
  void currentTimeInMillis(time_t) override {}
#if !defined(USE_WIN_DLL)
  void execDetailsProtoBuf(const protobuf::ExecutionDetails&) override {}
  void execDetailsEndProtoBuf(const protobuf::ExecutionDetailsEnd&) override {}
  void orderStatusProtoBuf(const protobuf::OrderStatus&) override {}
  void openOrderProtoBuf(const protobuf::OpenOrder&) override {}
  void openOrdersEndProtoBuf(const protobuf::OpenOrdersEnd&) override {}
  void errorProtoBuf(const protobuf::ErrorMessage&) override {}
#endif

 private:
  bool connected_ = false;
  IBKRCallbacks* callbacks_ = nullptr;
  EReaderOSSignal signal_{2000};
  EClientSocket client_{this, &signal_};
  EReader* reader_ = nullptr;
};

}  // namespace

std::unique_ptr<IBKRTransport> make_default_ibkr_transport() {
  return std::make_unique<RealIBKRTransport>();
}

}  // namespace hft
