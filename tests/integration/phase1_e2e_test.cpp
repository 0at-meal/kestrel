#include <gtest/gtest.h>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <quickfix/SessionID.h>
#include <quickfix/fix44/ExecutionReport.h>
#include <quickfix/fix44/NewOrderSingle.h>
#include <quickfix/fix44/OrderCancelRequest.h>

#include "kestrel/gateway/inbound_gateway.h"
#include "kestrel/gateway/outbound_gateway.h"
#include "kestrel/oms/oms_core.h"
#include "kestrel/venue/stub_venue.h"

using namespace kestrel::gateway;
using namespace kestrel::oms;
using namespace kestrel::venue;
using namespace kestrel::wire;

class Phase1PipelineHarness {
public:
    InboundOrderBuffer inbound_orders;
    InboundCancelBuffer inbound_cancels;
    OutboundExecBuffer client_exec_reports;

    OutboundOrderBuffer outbound_orders;
    OutboundCancelBuffer outbound_cancels;
    InboundExecBuffer venue_exec_reports;

    InboundGatewayApplication inbound_gw{inbound_orders, inbound_cancels, client_exec_reports};
    OmsCore oms_core{128, inbound_orders, inbound_cancels, client_exec_reports,
                     outbound_orders, outbound_cancels, venue_exec_reports};
    OutboundGatewayApplication outbound_gw{outbound_orders, outbound_cancels, venue_exec_reports};
    StubVenueApplication stub_venue;

    FIX::SessionID client_session{"FIX.4.4", "CLIENT", "KESTREL_INBOUND"};
    FIX::SessionID venue_session{"FIX.4.4", "KESTREL_OUTBOUND", "STUB_VENUE"};

    size_t forward_outbound_to_venue() {
        size_t count = 0;
        kestrel::wire::OrderEventWire order{};
        while (outbound_orders.try_pop(order)) {
            const char side_char = (order.side == Side::Buy) ? FIX::Side_BUY : FIX::Side_SELL;
            const char type_char = (order.order_type == OrderType::Market) ? FIX::OrdType_MARKET : FIX::OrdType_LIMIT;
            FIX44::NewOrderSingle nos(
                FIX::ClOrdID(order.cl_ord_id),
                FIX::Side(side_char),
                FIX::TransactTime(),
                FIX::OrdType(type_char)
            );
            nos.setField(FIX::Symbol(order.symbol));
            nos.setField(FIX::OrderQty(order.quantity));
            if (order.order_type == OrderType::Limit) {
                nos.setField(FIX::Price(order.price));
            }
            stub_venue.fromApp(nos, venue_session);
            ++count;
        }

        kestrel::wire::CancelEventWire cancel{};
        while (outbound_cancels.try_pop(cancel)) {
            FIX44::OrderCancelRequest req(
                FIX::OrigClOrdID(cancel.orig_cl_ord_id),
                FIX::ClOrdID(cancel.cl_ord_id),
                FIX::Side(FIX::Side_BUY),
                FIX::TransactTime()
            );
            stub_venue.fromApp(req, venue_session);
            ++count;
        }
        return count;
    }

    size_t forward_venue_to_outbound_gw() {
        const auto& reports = stub_venue.generated_reports();
        size_t count = reports.size();
        for (const auto& r : reports) {
            outbound_gw.fromApp(r, venue_session);
        }
        stub_venue.clear_generated_reports();
        return count;
    }
};

TEST(Phase1IntegrationTest, CompleteOrderLifecyclePendingNewToNewToFilled) {
    Phase1PipelineHarness h;

    // 1. Client submits NewOrderSingle via FIX
    FIX44::NewOrderSingle nos(
        FIX::ClOrdID("CL-001"),
        FIX::Side(FIX::Side_BUY),
        FIX::TransactTime(),
        FIX::OrdType(FIX::OrdType_LIMIT)
    );
    nos.setField(FIX::Symbol("BTC/USDT"));
    nos.setField(FIX::OrderQty(2.5));
    nos.setField(FIX::Price(65000.0));
    nos.setField(FIX::TimeInForce(FIX::TimeInForce_DAY));

    h.inbound_gw.fromApp(nos, h.client_session);
    EXPECT_EQ(h.inbound_gw.orders_received(), 1);

    // 2. OMS Core processes inbound order -> PendingNew -> routes to outbound
    EXPECT_EQ(h.oms_core.poll_once(), 1);
    EXPECT_EQ(h.oms_core.orders_processed(), 1);
    EXPECT_EQ(h.oms_core.order_pool().get(0).state, OrderState::PendingNew);

    // Client receives ExecutionReport (PendingNew)
    ExecReportEventWire er_pn{};
    ASSERT_TRUE(h.client_exec_reports.try_pop(er_pn));
    EXPECT_EQ(er_pn.order_id, 0);
    EXPECT_EQ(er_pn.ord_status, OrdStatus::PendingNew);
    EXPECT_DOUBLE_EQ(er_pn.leaves_qty, 2.5);
    EXPECT_DOUBLE_EQ(er_pn.cum_qty, 0.0);

    // 3. Outbound routing buffer forwarded to Stub Venue
    EXPECT_EQ(h.forward_outbound_to_venue(), 1);
    EXPECT_EQ(h.stub_venue.orders_received(), 1);

    // 4. Stub Venue generated Ack + Fill -> forward to Outbound Gateway
    EXPECT_EQ(h.forward_venue_to_outbound_gw(), 2);
    EXPECT_EQ(h.outbound_gw.reports_received(), 2);

    // 5. OMS Core processes Venue Ack -> state transitions to New
    EXPECT_EQ(h.oms_core.poll_once(), 1);
    EXPECT_EQ(h.oms_core.order_pool().get(0).state, OrderState::New);

    // Client receives ExecutionReport (New)
    ExecReportEventWire er_new{};
    ASSERT_TRUE(h.client_exec_reports.try_pop(er_new));
    EXPECT_EQ(er_new.order_id, 0);
    EXPECT_EQ(er_new.exec_type, ExecType::New);
    EXPECT_EQ(er_new.ord_status, OrdStatus::New);
    EXPECT_DOUBLE_EQ(er_new.leaves_qty, 2.5);
    EXPECT_DOUBLE_EQ(er_new.cum_qty, 0.0);

    // 6. OMS Core processes Venue Fill -> state transitions to Filled
    EXPECT_EQ(h.oms_core.poll_once(), 1);
    const Order& filled_order = h.oms_core.order_pool().get(0);
    EXPECT_EQ(filled_order.state, OrderState::Filled);
    EXPECT_DOUBLE_EQ(filled_order.cum_qty, 2.5);
    EXPECT_DOUBLE_EQ(filled_order.leaves_qty, 0.0);
    EXPECT_DOUBLE_EQ(filled_order.avg_px, 65000.0);
    EXPECT_TRUE(filled_order.check_quantity_invariant());

    // Client receives ExecutionReport (Filled)
    ExecReportEventWire er_fill{};
    ASSERT_TRUE(h.client_exec_reports.try_pop(er_fill));
    EXPECT_EQ(er_fill.order_id, 0);
    EXPECT_EQ(er_fill.exec_type, ExecType::Fill);
    EXPECT_EQ(er_fill.ord_status, OrdStatus::Filled);
    EXPECT_DOUBLE_EQ(er_fill.leaves_qty, 0.0);
    EXPECT_DOUBLE_EQ(er_fill.cum_qty, 2.5);
    EXPECT_DOUBLE_EQ(er_fill.last_qty, 2.5);
    EXPECT_DOUBLE_EQ(er_fill.last_px, 65000.0);

    // 7. Drain outbound exec reports through Inbound Gateway
    // Push report into buffer to verify drain_outbound translation
    EXPECT_TRUE(h.client_exec_reports.try_push(er_fill));
    size_t drained = h.inbound_gw.drain_outbound(h.client_session);
    EXPECT_EQ(drained, 0); // Caught SessionNotFound gracefully (no live socket)
    EXPECT_TRUE(h.client_exec_reports.empty_approx());
}

TEST(Phase1IntegrationTest, OrderCancelLifecycleE2E) {
    Phase1PipelineHarness h;

    // 1. Submit NewOrderSingle
    FIX44::NewOrderSingle nos(
        FIX::ClOrdID("CL-CANCEL-TEST"),
        FIX::Side(FIX::Side_SELL),
        FIX::TransactTime(),
        FIX::OrdType(FIX::OrdType_LIMIT)
    );
    nos.setField(FIX::Symbol("ETH/USDT"));
    nos.setField(FIX::OrderQty(10.0));
    nos.setField(FIX::Price(3000.0));

    h.inbound_gw.fromApp(nos, h.client_session);
    h.oms_core.poll_once(); // PendingNew
    h.forward_outbound_to_venue();

    // Venue sends Ack only (simulate unfilled order waiting at venue)
    h.stub_venue.clear_generated_reports();
    FIX44::ExecutionReport ack(
        FIX::OrderID("V-100"),
        FIX::ExecID("V-EXEC-1"),
        FIX::ExecType(FIX::ExecType_NEW),
        FIX::OrdStatus(FIX::OrdStatus_NEW),
        FIX::Side(FIX::Side_SELL),
        FIX::LeavesQty(10.0),
        FIX::CumQty(0.0),
        FIX::AvgPx(0.0)
    );
    ack.setField(FIX::ClOrdID("0"));
    h.outbound_gw.fromApp(ack, h.venue_session);
    h.oms_core.poll_once(); // Transitions to New

    EXPECT_EQ(h.oms_core.order_pool().get(0).state, OrderState::New);

    // 2. Client submits OrderCancelRequest
    FIX44::OrderCancelRequest cancel(
        FIX::OrigClOrdID("CL-CANCEL-TEST"),
        FIX::ClOrdID("CANC-REQ-1"),
        FIX::Side(FIX::Side_SELL),
        FIX::TransactTime()
    );
    h.inbound_gw.fromApp(cancel, h.client_session);
    EXPECT_EQ(h.inbound_gw.cancels_received(), 1);

    // 3. OMS Core transitions New -> PendingCancel
    h.oms_core.poll_once();
    EXPECT_EQ(h.oms_core.order_pool().get(0).state, OrderState::PendingCancel);

    // 4. Cancel routed to Stub Venue
    EXPECT_EQ(h.forward_outbound_to_venue(), 1);
    EXPECT_EQ(h.stub_venue.cancels_received(), 1);

    // 5. Venue sends Cancelled report -> forward to Outbound GW
    EXPECT_EQ(h.forward_venue_to_outbound_gw(), 1);

    // 6. OMS Core processes cancel ack -> state transitions to Cancelled
    h.oms_core.poll_once();
    const Order& cancelled_order = h.oms_core.order_pool().get(0);
    EXPECT_EQ(cancelled_order.state, OrderState::Cancelled);
    EXPECT_DOUBLE_EQ(cancelled_order.leaves_qty, 0.0);
    EXPECT_DOUBLE_EQ(cancelled_order.cum_qty, 0.0);
}

TEST(Phase1IntegrationTest, ConcurrentMultiThreadedPipelineE2E) {
    Phase1PipelineHarness h;
    constexpr int kNumOrders = 20;

    h.oms_core.start();
    EXPECT_TRUE(h.oms_core.is_running());

    // Submit orders concurrently
    std::thread producer([&h]() {
        for (int i = 0; i < kNumOrders; ++i) {
            FIX44::NewOrderSingle nos(
                FIX::ClOrdID("ASYNC-ORD-" + std::to_string(i)),
                FIX::Side(FIX::Side_BUY),
                FIX::TransactTime(),
                FIX::OrdType(FIX::OrdType_LIMIT)
            );
            nos.setField(FIX::Symbol("BTC/USDT"));
            nos.setField(FIX::OrderQty(1.0 + i));
            nos.setField(FIX::Price(50000.0 + i * 10.0));
            h.inbound_gw.fromApp(nos, h.client_session);
        }
    });

    // Pump between Outbound Gateway and Stub Venue
    std::thread pump([&h]() {
        size_t filled_count = 0;
        while (filled_count < kNumOrders) {
            h.forward_outbound_to_venue();
            h.forward_venue_to_outbound_gw();

            kestrel::wire::ExecReportEventWire er{};
            while (h.client_exec_reports.try_pop(er)) {
                if (er.ord_status == kestrel::wire::OrdStatus::Filled) {
                    ++filled_count;
                }
            }
            std::this_thread::yield();
        }
    });

    producer.join();
    pump.join();

    h.oms_core.stop();
    EXPECT_FALSE(h.oms_core.is_running());

    EXPECT_EQ(h.oms_core.orders_processed(), kNumOrders);

    // Verify all orders are in terminal Filled state with invariant valid
    for (size_t i = 0; i < kNumOrders; ++i) {
        const Order& order = h.oms_core.order_pool().get(i);
        EXPECT_EQ(order.state, OrderState::Filled);
        EXPECT_DOUBLE_EQ(order.leaves_qty, 0.0);
        EXPECT_GT(order.cum_qty, 0.0);
        EXPECT_TRUE(order.check_quantity_invariant());
    }
}

