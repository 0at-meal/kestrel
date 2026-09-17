#include <gtest/gtest.h>
#include <quickfix/SessionID.h>
#include <quickfix/fix44/ExecutionReport.h>
#include <quickfix/fix44/NewOrderSingle.h>
#include <quickfix/fix44/OrderCancelRequest.h>

#include "kestrel/gateway/outbound_gateway.h"
#include "kestrel/venue/stub_venue.h"

using namespace kestrel::gateway;
using namespace kestrel::venue;
using namespace kestrel::wire;

class OutboundGatewayTest : public ::testing::Test {
protected:
    OutboundOrderBuffer order_buf;
    OutboundCancelBuffer cancel_buf;
    InboundExecBuffer exec_buf;
    OutboundGatewayApplication gw{order_buf, cancel_buf, exec_buf};
    StubVenueApplication venue;
    FIX::SessionID sessionID{"FIX.4.4", "KESTREL_OMS", "VENUE"};
};

TEST_F(OutboundGatewayTest, SessionLifecycleCallbacks) {
    EXPECT_FALSE(gw.is_logged_on());
    gw.onLogon(sessionID);
    EXPECT_TRUE(gw.is_logged_on());
    gw.onLogout(sessionID);
    EXPECT_FALSE(gw.is_logged_on());

    EXPECT_FALSE(venue.is_logged_on());
    venue.onLogon(sessionID);
    EXPECT_TRUE(venue.is_logged_on());
    venue.onLogout(sessionID);
    EXPECT_FALSE(venue.is_logged_on());
}

TEST_F(OutboundGatewayTest, DrainOutboundOrders) {
    OrderEventWire o1{};
    std::strncpy(o1.cl_ord_id, "101", sizeof(o1.cl_ord_id));
    std::strncpy(o1.symbol, "BTC/USDT", sizeof(o1.symbol));
    o1.side = Side::Buy;
    o1.order_type = OrderType::Limit;
    o1.price = 50000.0;
    o1.quantity = 1.5;
    o1.time_in_force = TimeInForce::Day;

    OrderEventWire o2{};
    std::strncpy(o2.cl_ord_id, "102", sizeof(o2.cl_ord_id));
    std::strncpy(o2.symbol, "ETH/USDT", sizeof(o2.symbol));
    o2.side = Side::Sell;
    o2.order_type = OrderType::Market;
    o2.quantity = 10.0;
    o2.time_in_force = TimeInForce::GTC;

    EXPECT_TRUE(order_buf.try_push(o1));
    EXPECT_TRUE(order_buf.try_push(o2));
    EXPECT_EQ(order_buf.size_approx(), 2);

    // Draining without live network session catches SessionNotFound safely
    size_t sent = gw.drain_outbound(sessionID);
    EXPECT_EQ(sent, 0); // No live network socket in unit test
    EXPECT_TRUE(order_buf.empty_approx());
}

TEST_F(OutboundGatewayTest, DrainOutboundCancels) {
    CancelEventWire c1{};
    std::strncpy(c1.cl_ord_id, "CANC-1", sizeof(c1.cl_ord_id));
    std::strncpy(c1.orig_cl_ord_id, "101", sizeof(c1.orig_cl_ord_id));

    EXPECT_TRUE(cancel_buf.try_push(c1));
    EXPECT_EQ(cancel_buf.size_approx(), 1);

    size_t sent = gw.drain_outbound(sessionID);
    EXPECT_EQ(sent, 0);
    EXPECT_TRUE(cancel_buf.empty_approx());
}

TEST_F(OutboundGatewayTest, TranslateExecutionReportNew) {
    FIX44::ExecutionReport report(
        FIX::OrderID("V-ORD-1"),
        FIX::ExecID("EXEC-1001"),
        FIX::ExecType(FIX::ExecType_NEW),
        FIX::OrdStatus(FIX::OrdStatus_NEW),
        FIX::Side(FIX::Side_BUY),
        FIX::LeavesQty(2.5),
        FIX::CumQty(0.0),
        FIX::AvgPx(0.0)
    );
    report.setField(FIX::ClOrdID("42"));
    report.setField(FIX::Symbol("BTC/USDT"));

    gw.fromApp(report, sessionID);

    EXPECT_EQ(gw.reports_received(), 1);
    EXPECT_EQ(gw.validation_errors(), 0);

    ExecReportEventWire er{};
    ASSERT_TRUE(exec_buf.try_pop(er));
    EXPECT_STREQ(er.exec_id, "EXEC-1001");
    EXPECT_EQ(er.order_id, 42);
    EXPECT_EQ(er.exec_type, ExecType::New);
    EXPECT_EQ(er.ord_status, OrdStatus::New);
    EXPECT_DOUBLE_EQ(er.leaves_qty, 2.5);
    EXPECT_DOUBLE_EQ(er.cum_qty, 0.0);
    EXPECT_GT(er.mono_ts_ns, 0);
}

TEST_F(OutboundGatewayTest, TranslateExecutionReportFillWithPrefixId) {
    FIX44::ExecutionReport report(
        FIX::OrderID("V-ORD-2"),
        FIX::ExecID("EXEC-1002"),
        FIX::ExecType(FIX::ExecType_FILL),
        FIX::OrdStatus(FIX::OrdStatus_FILLED),
        FIX::Side(FIX::Side_BUY),
        FIX::LeavesQty(0.0),
        FIX::CumQty(2.5),
        FIX::AvgPx(65000.0)
    );
    report.setField(FIX::ClOrdID("ORD-105"));
    report.setField(FIX::Symbol("BTC/USDT"));
    report.setField(FIX::LastQty(2.5));
    report.setField(FIX::LastPx(65000.0));

    gw.fromApp(report, sessionID);

    EXPECT_EQ(gw.reports_received(), 1);
    EXPECT_EQ(gw.validation_errors(), 0);

    ExecReportEventWire er{};
    ASSERT_TRUE(exec_buf.try_pop(er));
    EXPECT_STREQ(er.exec_id, "EXEC-1002");
    EXPECT_EQ(er.order_id, 105);
    EXPECT_EQ(er.exec_type, ExecType::Fill);
    EXPECT_EQ(er.ord_status, OrdStatus::Filled);
    EXPECT_DOUBLE_EQ(er.leaves_qty, 0.0);
    EXPECT_DOUBLE_EQ(er.cum_qty, 2.5);
    EXPECT_DOUBLE_EQ(er.last_qty, 2.5);
    EXPECT_DOUBLE_EQ(er.last_px, 65000.0);
    EXPECT_GT(er.mono_ts_ns, 0);
}

TEST_F(OutboundGatewayTest, MalformedExecutionReportsRejectedSafely) {
    // Missing ExecID
    FIX44::ExecutionReport bad_rep(
        FIX::OrderID("V-ORD-3"),
        FIX::ExecID(""),
        FIX::ExecType(FIX::ExecType_NEW),
        FIX::OrdStatus(FIX::OrdStatus_NEW),
        FIX::Side(FIX::Side_BUY),
        FIX::LeavesQty(1.0),
        FIX::CumQty(0.0),
        FIX::AvgPx(0.0)
    );
    bad_rep.setField(FIX::ClOrdID("1"));

    gw.fromApp(bad_rep, sessionID);
    EXPECT_EQ(gw.reports_received(), 0);
    EXPECT_EQ(gw.validation_errors(), 1);
    EXPECT_TRUE(exec_buf.empty_approx());
}

TEST_F(OutboundGatewayTest, StubVenueGeneratesAckAndFill) {
    FIX44::NewOrderSingle nos(
        FIX::ClOrdID("CL-777"),
        FIX::Side(FIX::Side_BUY),
        FIX::TransactTime(),
        FIX::OrdType(FIX::OrdType_LIMIT)
    );
    nos.setField(FIX::Symbol("BTC/USDT"));
    nos.setField(FIX::OrderQty(3.0));
    nos.setField(FIX::Price(62000.0));

    venue.fromApp(nos, sessionID);

    EXPECT_EQ(venue.orders_received(), 1);
    const auto& reports = venue.generated_reports();
    ASSERT_EQ(reports.size(), 2);

    // 1. Check ACK
    const auto& ack = reports[0];
    EXPECT_EQ(ack.getField(FIX::FIELD::ExecType), std::string(1, FIX::ExecType_NEW));
    EXPECT_EQ(ack.getField(FIX::FIELD::OrdStatus), std::string(1, FIX::OrdStatus_NEW));
    EXPECT_EQ(ack.getField(FIX::FIELD::ClOrdID), "CL-777");
    EXPECT_DOUBLE_EQ(std::stod(ack.getField(FIX::FIELD::LeavesQty)), 3.0);
    EXPECT_DOUBLE_EQ(std::stod(ack.getField(FIX::FIELD::CumQty)), 0.0);

    // 2. Check Fill
    const auto& fill = reports[1];
    EXPECT_EQ(fill.getField(FIX::FIELD::ExecType), std::string(1, FIX::ExecType_FILL));
    EXPECT_EQ(fill.getField(FIX::FIELD::OrdStatus), std::string(1, FIX::OrdStatus_FILLED));
    EXPECT_EQ(fill.getField(FIX::FIELD::ClOrdID), "CL-777");
    EXPECT_DOUBLE_EQ(std::stod(fill.getField(FIX::FIELD::LeavesQty)), 0.0);
    EXPECT_DOUBLE_EQ(std::stod(fill.getField(FIX::FIELD::CumQty)), 3.0);
    EXPECT_DOUBLE_EQ(std::stod(fill.getField(FIX::FIELD::LastQty)), 3.0);
    EXPECT_DOUBLE_EQ(std::stod(fill.getField(FIX::FIELD::LastPx)), 62000.0);
}

TEST_F(OutboundGatewayTest, StubVenueGeneratesCancelReport) {
    FIX44::OrderCancelRequest cancel(
        FIX::OrigClOrdID("CL-777"),
        FIX::ClOrdID("CANC-99"),
        FIX::Side(FIX::Side_BUY),
        FIX::TransactTime()
    );

    venue.fromApp(cancel, sessionID);

    EXPECT_EQ(venue.cancels_received(), 1);
    const auto& reports = venue.generated_reports();
    ASSERT_EQ(reports.size(), 1);

    const auto& cr = reports[0];
    EXPECT_EQ(cr.getField(FIX::FIELD::ExecType), std::string(1, FIX::ExecType_CANCELED));
    EXPECT_EQ(cr.getField(FIX::FIELD::OrdStatus), std::string(1, FIX::OrdStatus_CANCELED));
    EXPECT_EQ(cr.getField(FIX::FIELD::OrigClOrdID), "CL-777");
    EXPECT_EQ(cr.getField(FIX::FIELD::ClOrdID), "CANC-99");
}

TEST_F(OutboundGatewayTest, EndToEndHandoffStubVenueToOutboundGateway) {
    FIX44::NewOrderSingle nos(
        FIX::ClOrdID("888"),
        FIX::Side(FIX::Side_SELL),
        FIX::TransactTime(),
        FIX::OrdType(FIX::OrdType_LIMIT)
    );
    nos.setField(FIX::Symbol("ETH/USDT"));
    nos.setField(FIX::OrderQty(5.0));
    nos.setField(FIX::Price(3500.0));

    venue.fromApp(nos, sessionID);
    const auto& reports = venue.generated_reports();
    ASSERT_EQ(reports.size(), 2);

    // Feed reports into Outbound Gateway
    gw.fromApp(reports[0], sessionID);
    gw.fromApp(reports[1], sessionID);

    EXPECT_EQ(gw.reports_received(), 2);
    EXPECT_EQ(gw.validation_errors(), 0);

    // Verify ExecReportEventWire stream in exec_buf
    ExecReportEventWire er_ack{};
    ASSERT_TRUE(exec_buf.try_pop(er_ack));
    EXPECT_EQ(er_ack.order_id, 888);
    EXPECT_EQ(er_ack.exec_type, ExecType::New);
    EXPECT_EQ(er_ack.ord_status, OrdStatus::New);
    EXPECT_DOUBLE_EQ(er_ack.leaves_qty, 5.0);
    EXPECT_DOUBLE_EQ(er_ack.cum_qty, 0.0);

    ExecReportEventWire er_fill{};
    ASSERT_TRUE(exec_buf.try_pop(er_fill));
    EXPECT_EQ(er_fill.order_id, 888);
    EXPECT_EQ(er_fill.exec_type, ExecType::Fill);
    EXPECT_EQ(er_fill.ord_status, OrdStatus::Filled);
    EXPECT_DOUBLE_EQ(er_fill.leaves_qty, 0.0);
    EXPECT_DOUBLE_EQ(er_fill.cum_qty, 5.0);
    EXPECT_DOUBLE_EQ(er_fill.last_qty, 5.0);
    EXPECT_DOUBLE_EQ(er_fill.last_px, 3500.0);
}
