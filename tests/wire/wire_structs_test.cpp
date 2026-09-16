#include <gtest/gtest.h>
#include <cstring>
#include <type_traits>
#include "kestrel/wire/enums.h"
#include "kestrel/wire/wire_structs.h"

using namespace kestrel::wire;

TEST(WireEnumsTest, UnderlyingTypeAndSentinels) {
    EXPECT_EQ(sizeof(Side), 1);
    EXPECT_EQ(sizeof(OrderType), 1);
    EXPECT_EQ(sizeof(TimeInForce), 1);
    EXPECT_EQ(sizeof(ExecType), 1);
    EXPECT_EQ(sizeof(OrdStatus), 1);

    EXPECT_EQ(static_cast<uint8_t>(Side::Count), 2);
    EXPECT_EQ(static_cast<uint8_t>(OrderType::Count), 2);
    EXPECT_EQ(static_cast<uint8_t>(TimeInForce::Count), 2);
    EXPECT_EQ(static_cast<uint8_t>(ExecType::Count), 8);
    EXPECT_EQ(static_cast<uint8_t>(OrdStatus::Count), 11);

    EXPECT_EQ(to_string(Side::Buy), "Buy");
    EXPECT_EQ(to_string(Side::Sell), "Sell");
    EXPECT_EQ(to_string(OrderType::Market), "Market");
    EXPECT_EQ(to_string(OrderType::Limit), "Limit");
    EXPECT_EQ(to_string(TimeInForce::Day), "Day");
    EXPECT_EQ(to_string(TimeInForce::GTC), "GTC");
    EXPECT_EQ(to_string(ExecType::Fill), "Fill");
    EXPECT_EQ(to_string(OrdStatus::Filled), "Filled");
}

TEST(WireStructsTest, StandardLayoutAndTriviallyCopyable) {
    EXPECT_TRUE(std::is_standard_layout_v<OrderEventWire>);
    EXPECT_TRUE(std::is_trivially_copyable_v<OrderEventWire>);

    EXPECT_TRUE(std::is_standard_layout_v<CancelEventWire>);
    EXPECT_TRUE(std::is_trivially_copyable_v<CancelEventWire>);

    EXPECT_TRUE(std::is_standard_layout_v<ExecReportEventWire>);
    EXPECT_TRUE(std::is_trivially_copyable_v<ExecReportEventWire>);

    EXPECT_TRUE(std::is_standard_layout_v<MarketDataEventWire>);
    EXPECT_TRUE(std::is_trivially_copyable_v<MarketDataEventWire>);
}

TEST(WireStructsTest, FieldSizesAndCopySemantics) {
    OrderEventWire order{};
    std::strncpy(order.cl_ord_id, "ORD-1234567890", sizeof(order.cl_ord_id));
    std::strncpy(order.symbol, "BTC/USDT", sizeof(order.symbol));
    order.side = Side::Buy;
    order.order_type = OrderType::Limit;
    order.price = 50000.50;
    order.quantity = 1.25;
    order.session_id = 42;
    order.mono_ts_ns = 1234567890123ULL;

    OrderEventWire copy_order = order;
    EXPECT_STREQ(copy_order.cl_ord_id, "ORD-1234567890");
    EXPECT_STREQ(copy_order.symbol, "BTC/USDT");
    EXPECT_EQ(copy_order.side, Side::Buy);
    EXPECT_EQ(copy_order.price, 50000.50);
    EXPECT_EQ(copy_order.quantity, 1.25);
    EXPECT_EQ(copy_order.session_id, 42);
    EXPECT_EQ(copy_order.mono_ts_ns, 1234567890123ULL);

    // CancelEventWire check
    CancelEventWire cancel{};
    std::strncpy(cancel.cl_ord_id, "CANC-1", sizeof(cancel.cl_ord_id));
    std::strncpy(cancel.orig_cl_ord_id, "ORD-1234567890", sizeof(cancel.orig_cl_ord_id));
    cancel.session_id = 42;
    cancel.mono_ts_ns = 1234567890200ULL;
    CancelEventWire copy_cancel = cancel;
    EXPECT_STREQ(copy_cancel.cl_ord_id, "CANC-1");
    EXPECT_STREQ(copy_cancel.orig_cl_ord_id, "ORD-1234567890");

    // ExecReportEventWire check
    ExecReportEventWire er{};
    std::strncpy(er.exec_id, "EXEC-999", sizeof(er.exec_id));
    er.order_id = 1001;
    er.exec_type = ExecType::Fill;
    er.ord_status = OrdStatus::Filled;
    er.last_qty = 1.25;
    er.last_px = 50000.50;
    er.cum_qty = 1.25;
    er.leaves_qty = 0.0;
    er.mono_ts_ns = 1234567890300ULL;
    ExecReportEventWire copy_er = er;
    EXPECT_STREQ(copy_er.exec_id, "EXEC-999");
    EXPECT_EQ(copy_er.order_id, 1001);
    EXPECT_EQ(copy_er.exec_type, ExecType::Fill);
    EXPECT_EQ(copy_er.ord_status, OrdStatus::Filled);

    // MarketDataEventWire check
    MarketDataEventWire md{};
    std::strncpy(md.symbol, "ETH/USDT", sizeof(md.symbol));
    md.bid_px = 3000.0;
    md.ask_px = 3000.5;
    md.last_px = 3000.25;
    md.mono_ts_ns = 1234567890400ULL;
    MarketDataEventWire copy_md = md;
    EXPECT_STREQ(copy_md.symbol, "ETH/USDT");
    EXPECT_DOUBLE_EQ(copy_md.bid_px, 3000.0);
    EXPECT_DOUBLE_EQ(copy_md.ask_px, 3000.5);
    EXPECT_DOUBLE_EQ(copy_md.last_px, 3000.25);
}
