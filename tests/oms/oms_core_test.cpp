#include <gtest/gtest.h>
#include <chrono>
#include <cstring>
#include <thread>

#include "kestrel/gateway/inbound_gateway.h"
#include "kestrel/gateway/outbound_gateway.h"
#include "kestrel/oms/oms_core.h"

using namespace kestrel::gateway;
using namespace kestrel::oms;
using namespace kestrel::wire;

class OmsCoreTest : public ::testing::Test {
protected:
    InboundOrderBuffer inbound_orders;
    InboundCancelBuffer inbound_cancels;
    OutboundExecBuffer client_exec_reports;

    OutboundOrderBuffer outbound_orders;
    OutboundCancelBuffer outbound_cancels;
    InboundExecBuffer venue_exec_reports;

    OmsCore oms{16, inbound_orders, inbound_cancels, client_exec_reports,
                outbound_orders, outbound_cancels, venue_exec_reports};
};

TEST_F(OmsCoreTest, NewOrderAllocationAndPendingNew) {
    OrderEventWire wire{};
    std::strncpy(wire.cl_ord_id, "ORD-1", sizeof(wire.cl_ord_id));
    std::strncpy(wire.symbol, "BTC/USDT", sizeof(wire.symbol));
    wire.side = Side::Buy;
    wire.order_type = OrderType::Limit;
    wire.time_in_force = TimeInForce::Day;
    wire.price = 50000.0;
    wire.quantity = 2.0;
    wire.session_id = 1;
    wire.mono_ts_ns = OmsCore::current_mono_ns();

    EXPECT_TRUE(inbound_orders.try_push(wire));
    EXPECT_EQ(oms.poll_once(), 1);

    EXPECT_EQ(oms.orders_processed(), 1);
    EXPECT_EQ(oms.orders_rejected(), 0);
    EXPECT_EQ(oms.order_pool().size(), 1);

    const Order& order = oms.order_pool().get(0);
    EXPECT_EQ(order.order_id, 0);
    EXPECT_STREQ(order.cl_ord_id, "ORD-1");
    EXPECT_EQ(order.state, OrderState::PendingNew);
    EXPECT_DOUBLE_EQ(order.quantity, 2.0);
    EXPECT_DOUBLE_EQ(order.leaves_qty, 2.0);
    EXPECT_DOUBLE_EQ(order.cum_qty, 0.0);
    EXPECT_TRUE(order.check_quantity_invariant());

    // Client execution report check (FR-016: transition to PendingNew)
    ExecReportEventWire client_rep{};
    ASSERT_TRUE(client_exec_reports.try_pop(client_rep));
    EXPECT_EQ(client_rep.order_id, 0);
    EXPECT_EQ(client_rep.ord_status, OrdStatus::PendingNew);
    EXPECT_DOUBLE_EQ(client_rep.leaves_qty, 2.0);
    EXPECT_DOUBLE_EQ(client_rep.cum_qty, 0.0);

    // Outbound routing buffer check
    OrderEventWire routed{};
    ASSERT_TRUE(outbound_orders.try_pop(routed));
    EXPECT_STREQ(routed.cl_ord_id, "ORD-1");
    EXPECT_DOUBLE_EQ(routed.quantity, 2.0);

    EXPECT_EQ(oms.latency_histogram().total_count(), 1);
}

TEST_F(OmsCoreTest, DuplicateClOrdIdRejected) {
    OrderEventWire o1{};
    std::strncpy(o1.cl_ord_id, "CL-DUP", sizeof(o1.cl_ord_id));
    std::strncpy(o1.symbol, "BTC/USDT", sizeof(o1.symbol));
    o1.side = Side::Buy;
    o1.quantity = 1.0;
    o1.price = 100.0;
    o1.mono_ts_ns = OmsCore::current_mono_ns();

    EXPECT_TRUE(inbound_orders.try_push(o1));
    EXPECT_EQ(oms.poll_once(), 1);
    EXPECT_EQ(oms.orders_processed(), 1);

    // Duplicate submission
    OrderEventWire o2 = o1;
    o2.mono_ts_ns = OmsCore::current_mono_ns();
    EXPECT_TRUE(inbound_orders.try_push(o2));
    EXPECT_EQ(oms.poll_once(), 1);

    EXPECT_EQ(oms.orders_processed(), 1);
    EXPECT_EQ(oms.orders_rejected(), 1);

    // First report is PendingNew, second report must be Rejected
    ExecReportEventWire rep1{}, rep2{};
    ASSERT_TRUE(client_exec_reports.try_pop(rep1));
    ASSERT_TRUE(client_exec_reports.try_pop(rep2));
    EXPECT_EQ(rep1.ord_status, OrdStatus::PendingNew);
    EXPECT_EQ(rep2.exec_type, ExecType::Rejected);
    EXPECT_EQ(rep2.ord_status, OrdStatus::Rejected);
}

TEST_F(OmsCoreTest, PoolExhaustionRejection) {
    OmsCore tiny_oms{2, inbound_orders, inbound_cancels, client_exec_reports,
                     outbound_orders, outbound_cancels, venue_exec_reports};

    for (int i = 0; i < 2; ++i) {
        OrderEventWire o{};
        std::snprintf(o.cl_ord_id, sizeof(o.cl_ord_id), "ID-%d", i);
        std::strncpy(o.symbol, "BTC/USDT", sizeof(o.symbol));
        o.quantity = 1.0;
        o.price = 100.0;
        EXPECT_TRUE(inbound_orders.try_push(o));
        tiny_oms.poll_once();
    }
    EXPECT_EQ(tiny_oms.orders_processed(), 2);
    EXPECT_EQ(tiny_oms.orders_rejected(), 0);

    // Third order exceeds capacity 2
    OrderEventWire o3{};
    std::strncpy(o3.cl_ord_id, "ID-3", sizeof(o3.cl_ord_id));
    std::strncpy(o3.symbol, "BTC/USDT", sizeof(o3.symbol));
    o3.quantity = 1.0;
    o3.price = 100.0;
    EXPECT_TRUE(inbound_orders.try_push(o3));
    tiny_oms.poll_once();

    EXPECT_EQ(tiny_oms.orders_processed(), 2);
    EXPECT_EQ(tiny_oms.orders_rejected(), 1);
}

TEST_F(OmsCoreTest, VenueAckTransitionsToNew) {
    OrderEventWire o{};
    std::strncpy(o.cl_ord_id, "ORD-ACK", sizeof(o.cl_ord_id));
    std::strncpy(o.symbol, "BTC/USDT", sizeof(o.symbol));
    o.quantity = 5.0;
    o.price = 50000.0;
    EXPECT_TRUE(inbound_orders.try_push(o));
    oms.poll_once();

    // Drain initial reports
    ExecReportEventWire dummy_er{};
    EXPECT_TRUE(client_exec_reports.try_pop(dummy_er));
    OrderEventWire dummy_o{};
    EXPECT_TRUE(outbound_orders.try_pop(dummy_o));

    // Venue sends Ack
    ExecReportEventWire venue_ack{};
    std::strncpy(venue_ack.exec_id, "V-ACK-1", sizeof(venue_ack.exec_id));
    venue_ack.order_id = 0;
    venue_ack.exec_type = ExecType::New;
    venue_ack.ord_status = OrdStatus::New;
    venue_ack.leaves_qty = 5.0;
    venue_ack.cum_qty = 0.0;
    venue_ack.mono_ts_ns = OmsCore::current_mono_ns();

    EXPECT_TRUE(venue_exec_reports.try_push(venue_ack));
    EXPECT_EQ(oms.poll_once(), 1);
    EXPECT_EQ(oms.reports_processed(), 1);

    const Order& order = oms.order_pool().get(0);
    EXPECT_EQ(order.state, OrderState::New);

    ExecReportEventWire client_rep{};
    ASSERT_TRUE(client_exec_reports.try_pop(client_rep));
    EXPECT_EQ(client_rep.order_id, 0);
    EXPECT_EQ(client_rep.exec_type, ExecType::New);
    EXPECT_EQ(client_rep.ord_status, OrdStatus::New);
}

TEST_F(OmsCoreTest, VenuePartialAndFullFill) {
    OrderEventWire o{};
    std::strncpy(o.cl_ord_id, "ORD-FILL", sizeof(o.cl_ord_id));
    std::strncpy(o.symbol, "BTC/USDT", sizeof(o.symbol));
    o.quantity = 100.0;
    o.price = 50.0;
    EXPECT_TRUE(inbound_orders.try_push(o));
    oms.poll_once();

    // Venue Ack -> New
    ExecReportEventWire ack{};
    ack.order_id = 0;
    ack.exec_type = ExecType::New;
    ack.ord_status = OrdStatus::New;
    ack.leaves_qty = 100.0;
    EXPECT_TRUE(venue_exec_reports.try_push(ack));
    EXPECT_EQ(oms.poll_once(), 1);

    // 1. Partial fill: 40 @ 45.0
    ExecReportEventWire pfill{};
    std::strncpy(pfill.exec_id, "V-PF-1", sizeof(pfill.exec_id));
    pfill.order_id = 0;
    pfill.exec_type = ExecType::PartialFill;
    pfill.ord_status = OrdStatus::PartiallyFilled;
    pfill.last_qty = 40.0;
    pfill.last_px = 45.0;
    pfill.cum_qty = 40.0;
    pfill.leaves_qty = 60.0;
    pfill.mono_ts_ns = OmsCore::current_mono_ns();

    EXPECT_TRUE(venue_exec_reports.try_push(pfill));
    EXPECT_EQ(oms.poll_once(), 1);

    const Order& order1 = oms.order_pool().get(0);
    EXPECT_EQ(order1.state, OrderState::PartiallyFilled);
    EXPECT_DOUBLE_EQ(order1.cum_qty, 40.0);
    EXPECT_DOUBLE_EQ(order1.leaves_qty, 60.0);
    EXPECT_DOUBLE_EQ(order1.avg_px, 45.0);
    EXPECT_TRUE(order1.check_quantity_invariant());

    // 2. Full fill: 60 @ 55.0
    ExecReportEventWire ffill{};
    std::strncpy(ffill.exec_id, "V-FF-1", sizeof(ffill.exec_id));
    ffill.order_id = 0;
    ffill.exec_type = ExecType::Fill;
    ffill.ord_status = OrdStatus::Filled;
    ffill.last_qty = 60.0;
    ffill.last_px = 55.0;
    ffill.cum_qty = 100.0;
    ffill.leaves_qty = 0.0;
    ffill.mono_ts_ns = OmsCore::current_mono_ns();

    EXPECT_TRUE(venue_exec_reports.try_push(ffill));
    EXPECT_EQ(oms.poll_once(), 1);

    const Order& order2 = oms.order_pool().get(0);
    EXPECT_EQ(order2.state, OrderState::Filled);
    EXPECT_DOUBLE_EQ(order2.cum_qty, 100.0);
    EXPECT_DOUBLE_EQ(order2.leaves_qty, 0.0);
    // (40*45 + 60*55) / 100 = (1800 + 3300) / 100 = 51.0
    EXPECT_DOUBLE_EQ(order2.avg_px, 51.0);
    EXPECT_TRUE(order2.check_quantity_invariant());
}

TEST_F(OmsCoreTest, CancelLifecycleAndTerminalTransition) {
    OrderEventWire o{};
    std::strncpy(o.cl_ord_id, "ORD-CANC", sizeof(o.cl_ord_id));
    std::strncpy(o.symbol, "BTC/USDT", sizeof(o.symbol));
    o.quantity = 10.0;
    o.price = 100.0;
    EXPECT_TRUE(inbound_orders.try_push(o));
    oms.poll_once();

    // Venue Ack -> New
    ExecReportEventWire ack{};
    ack.order_id = 0;
    ack.exec_type = ExecType::New;
    ack.ord_status = OrdStatus::New;
    ack.leaves_qty = 10.0;
    EXPECT_TRUE(venue_exec_reports.try_push(ack));
    oms.poll_once();

    // Client requests cancel
    CancelEventWire cancel{};
    std::strncpy(cancel.cl_ord_id, "CANC-1", sizeof(cancel.cl_ord_id));
    std::strncpy(cancel.orig_cl_ord_id, "ORD-CANC", sizeof(cancel.orig_cl_ord_id));
    cancel.mono_ts_ns = OmsCore::current_mono_ns();

    EXPECT_TRUE(inbound_cancels.try_push(cancel));
    EXPECT_EQ(oms.poll_once(), 1);
    EXPECT_EQ(oms.cancels_processed(), 1);

    const Order& order = oms.order_pool().get(0);
    EXPECT_EQ(order.state, OrderState::PendingCancel);

    // Cancel routed to venue
    CancelEventWire routed_cancel{};
    ASSERT_TRUE(outbound_cancels.try_pop(routed_cancel));
    EXPECT_STREQ(routed_cancel.orig_cl_ord_id, "ORD-CANC");

    // Venue confirms cancel
    ExecReportEventWire venue_cancel{};
    std::strncpy(venue_cancel.exec_id, "V-CANC-ACK", sizeof(venue_cancel.exec_id));
    venue_cancel.order_id = 0;
    venue_cancel.exec_type = ExecType::Cancelled;
    venue_cancel.ord_status = OrdStatus::Cancelled;
    venue_cancel.leaves_qty = 0.0;
    venue_cancel.cum_qty = 0.0;
    EXPECT_TRUE(venue_exec_reports.try_push(venue_cancel));
    EXPECT_EQ(oms.poll_once(), 1);

    EXPECT_EQ(oms.order_pool().get(0).state, OrderState::Cancelled);
    EXPECT_DOUBLE_EQ(oms.order_pool().get(0).leaves_qty, 0.0);
}

TEST_F(OmsCoreTest, BackgroundThreadProcessing) {
    oms.start();
    EXPECT_TRUE(oms.is_running());

    OrderEventWire o{};
    std::strncpy(o.cl_ord_id, "ASYNC-1", sizeof(o.cl_ord_id));
    std::strncpy(o.symbol, "BTC/USDT", sizeof(o.symbol));
    o.quantity = 1.0;
    o.price = 100.0;
    o.mono_ts_ns = OmsCore::current_mono_ns();

    EXPECT_TRUE(inbound_orders.try_push(o));

    // Wait briefly for background thread to drain
    for (int i = 0; i < 100 && oms.orders_processed() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    EXPECT_EQ(oms.orders_processed(), 1);
    oms.stop();
    EXPECT_FALSE(oms.is_running());
}
