#include <gtest/gtest.h>
#include <quickfix/SessionID.h>
#include <quickfix/fix44/NewOrderSingle.h>
#include <quickfix/fix44/OrderCancelRequest.h>
#include "kestrel/gateway/inbound_gateway.h"

using namespace kestrel::gateway;
using namespace kestrel::wire;

class InboundGatewayTest : public ::testing::Test {
protected:
    InboundOrderBuffer order_buf;
    InboundCancelBuffer cancel_buf;
    OutboundExecBuffer exec_buf;
    InboundGatewayApplication app{order_buf, cancel_buf, exec_buf};
    FIX::SessionID sessionID{"FIX.4.4", "CLIENT", "KESTREL"};
};

TEST_F(InboundGatewayTest, SessionLifecycleCallbacks) {
    EXPECT_FALSE(app.is_logged_on());
    app.onLogon(sessionID);
    EXPECT_TRUE(app.is_logged_on());
    app.onLogout(sessionID);
    EXPECT_FALSE(app.is_logged_on());
}

TEST_F(InboundGatewayTest, TranslateNewOrderSingleLimitBuy) {
    FIX44::NewOrderSingle msg(
        FIX::ClOrdID("CL-001"),
        FIX::Side(FIX::Side_BUY),
        FIX::TransactTime(),
        FIX::OrdType(FIX::OrdType_LIMIT)
    );
    msg.setField(FIX::Symbol("BTC/USDT"));
    msg.setField(FIX::OrderQty(2.5));
    msg.setField(FIX::Price(65000.50));
    msg.setField(FIX::TimeInForce(FIX::TimeInForce_DAY));

    app.fromApp(msg, sessionID);

    EXPECT_EQ(app.orders_received(), 1);
    EXPECT_EQ(app.validation_errors(), 0);

    OrderEventWire wire{};
    ASSERT_TRUE(order_buf.try_pop(wire));
    EXPECT_STREQ(wire.cl_ord_id, "CL-001");
    EXPECT_STREQ(wire.symbol, "BTC/USDT");
    EXPECT_EQ(wire.side, Side::Buy);
    EXPECT_EQ(wire.order_type, OrderType::Limit);
    EXPECT_EQ(wire.time_in_force, TimeInForce::Day);
    EXPECT_DOUBLE_EQ(wire.quantity, 2.5);
    EXPECT_DOUBLE_EQ(wire.price, 65000.50);
    EXPECT_GT(wire.mono_ts_ns, 0);
}

TEST_F(InboundGatewayTest, TranslateNewOrderSingleMarketSell) {
    FIX44::NewOrderSingle msg(
        FIX::ClOrdID("CL-002"),
        FIX::Side(FIX::Side_SELL),
        FIX::TransactTime(),
        FIX::OrdType(FIX::OrdType_MARKET)
    );
    msg.setField(FIX::Symbol("ETH/USDT"));
    msg.setField(FIX::OrderQty(10.0));
    msg.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));

    app.fromApp(msg, sessionID);

    EXPECT_EQ(app.orders_received(), 1);
    EXPECT_EQ(app.validation_errors(), 0);

    OrderEventWire wire{};
    ASSERT_TRUE(order_buf.try_pop(wire));
    EXPECT_STREQ(wire.cl_ord_id, "CL-002");
    EXPECT_STREQ(wire.symbol, "ETH/USDT");
    EXPECT_EQ(wire.side, Side::Sell);
    EXPECT_EQ(wire.order_type, OrderType::Market);
    EXPECT_EQ(wire.time_in_force, TimeInForce::GTC);
    EXPECT_DOUBLE_EQ(wire.quantity, 10.0);
    EXPECT_DOUBLE_EQ(wire.price, 0.0);
    EXPECT_GT(wire.mono_ts_ns, 0);
}

TEST_F(InboundGatewayTest, TranslateOrderCancelRequest) {
    FIX44::OrderCancelRequest msg(
        FIX::OrigClOrdID("CL-001"),
        FIX::ClOrdID("CANC-001"),
        FIX::Side(FIX::Side_BUY),
        FIX::TransactTime()
    );
    msg.setField(FIX::Symbol("BTC/USDT"));

    app.fromApp(msg, sessionID);

    EXPECT_EQ(app.cancels_received(), 1);
    EXPECT_EQ(app.validation_errors(), 0);

    CancelEventWire wire{};
    ASSERT_TRUE(cancel_buf.try_pop(wire));
    EXPECT_STREQ(wire.cl_ord_id, "CANC-001");
    EXPECT_STREQ(wire.orig_cl_ord_id, "CL-001");
    EXPECT_GT(wire.mono_ts_ns, 0);
}

TEST_F(InboundGatewayTest, MalformedMessagesRejectedSafely) {
    // 1. Missing Symbol
    FIX44::NewOrderSingle msg1(
        FIX::ClOrdID("BAD-1"),
        FIX::Side(FIX::Side_BUY),
        FIX::TransactTime(),
        FIX::OrdType(FIX::OrdType_LIMIT)
    );
    msg1.setField(FIX::OrderQty(1.0));
    msg1.setField(FIX::Price(100.0));
    app.fromApp(msg1, sessionID);

    // 2. Limit order missing price
    FIX44::NewOrderSingle msg2(
        FIX::ClOrdID("BAD-2"),
        FIX::Side(FIX::Side_BUY),
        FIX::TransactTime(),
        FIX::OrdType(FIX::OrdType_LIMIT)
    );
    msg2.setField(FIX::Symbol("BTC/USDT"));
    msg2.setField(FIX::OrderQty(1.0));
    app.fromApp(msg2, sessionID);

    // 3. Non-positive quantity
    FIX44::NewOrderSingle msg3(
        FIX::ClOrdID("BAD-3"),
        FIX::Side(FIX::Side_BUY),
        FIX::TransactTime(),
        FIX::OrdType(FIX::OrdType_MARKET)
    );
    msg3.setField(FIX::Symbol("BTC/USDT"));
    msg3.setField(FIX::OrderQty(-5.0));
    app.fromApp(msg3, sessionID);

    EXPECT_EQ(app.orders_received(), 0);
    EXPECT_EQ(app.validation_errors(), 3);
    EXPECT_TRUE(order_buf.empty_approx());
}

TEST_F(InboundGatewayTest, DrainOutboundExecReport) {
    ExecReportEventWire er{};
    std::strncpy(er.exec_id, "EXEC-1", sizeof(er.exec_id));
    er.order_id = 100;
    er.exec_type = ExecType::New;
    er.ord_status = OrdStatus::New;
    er.leaves_qty = 5.0;
    er.cum_qty = 0.0;
    er.last_px = 50.0;
    er.last_qty = 0.0;

    EXPECT_TRUE(exec_buf.try_push(er));
    EXPECT_EQ(exec_buf.size_approx(), 1);

    // Draining without active network session safely pops and attempts send
    // (catches SessionNotFound)
    size_t drained = app.drain_outbound(sessionID);
    EXPECT_EQ(drained, 0); // No active network session in unit test
    EXPECT_EQ(exec_buf.size_approx(), 0); // Buffer was drained
}
