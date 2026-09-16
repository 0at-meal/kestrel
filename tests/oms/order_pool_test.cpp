#include <gtest/gtest.h>
#include "kestrel/oms/order_pool.h"

using namespace kestrel::oms;

TEST(OrderPoolTest, BasicAllocationAndAccess) {
    OrderPool pool(10);
    EXPECT_EQ(pool.capacity(), 10);
    EXPECT_EQ(pool.size(), 0);
    EXPECT_FALSE(pool.is_full());

    auto id0 = pool.allocate();
    ASSERT_TRUE(id0.has_value());
    EXPECT_EQ(*id0, 0);
    EXPECT_EQ(pool.size(), 1);

    Order& o0 = pool.get(*id0);
    EXPECT_EQ(o0.order_id, 0);
    o0.price = 100.0;
    o0.quantity = 10.0;
    o0.leaves_qty = 10.0;
    EXPECT_TRUE(o0.check_quantity_invariant());

    auto id1 = pool.allocate();
    ASSERT_TRUE(id1.has_value());
    EXPECT_EQ(*id1, 1);
    EXPECT_EQ(pool.size(), 2);
}

TEST(OrderPoolTest, PoolExhaustion) {
    constexpr size_t Cap = 3;
    OrderPool pool(Cap);

    auto id0 = pool.allocate();
    auto id1 = pool.allocate();
    auto id2 = pool.allocate();
    ASSERT_TRUE(id0.has_value());
    ASSERT_TRUE(id1.has_value());
    ASSERT_TRUE(id2.has_value());
    EXPECT_TRUE(pool.is_full());

    // Next allocation must return nullopt, not crash
    auto id3 = pool.allocate();
    EXPECT_FALSE(id3.has_value());
}

TEST(OrderPoolTest, ClOrdIdDuplicateDetection) {
    OrderPool pool(5);

    auto id0 = pool.allocate();
    ASSERT_TRUE(id0.has_value());

    // Register first ClOrdID
    EXPECT_TRUE(pool.register_cl_ord_id("ORD-001", *id0));

    // Lookup should succeed
    auto found = pool.find_by_cl_ord_id("ORD-001");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, *id0);

    // Duplicate registration should fail
    auto id1 = pool.allocate();
    ASSERT_TRUE(id1.has_value());
    EXPECT_FALSE(pool.register_cl_ord_id("ORD-001", *id1));

    // Original registration still valid
    EXPECT_EQ(pool.find_by_cl_ord_id("ORD-001"), *id0);

    // Non-existent ClOrdID lookup
    EXPECT_FALSE(pool.find_by_cl_ord_id("UNKNOWN").has_value());
}

TEST(OrderPoolTest, OrderFillTrackingAndQuantityInvariant) {
    Order order{};
    order.order_id = 1;
    order.quantity = 100.0;
    order.leaves_qty = 100.0;
    order.cum_qty = 0.0;
    order.avg_px = 0.0;
    EXPECT_TRUE(order.check_quantity_invariant());

    // Partial fill 1: 40 @ 10.0
    order.apply_fill(40.0, 10.0, 1000);
    EXPECT_DOUBLE_EQ(order.cum_qty, 40.0);
    EXPECT_DOUBLE_EQ(order.leaves_qty, 60.0);
    EXPECT_DOUBLE_EQ(order.avg_px, 10.0);
    EXPECT_EQ(order.updated_at_mono_ns, 1000);
    EXPECT_TRUE(order.check_quantity_invariant());

    // Partial fill 2: 60 @ 20.0
    // Total notional = 40 * 10 + 60 * 20 = 400 + 1200 = 1600. Avg price = 16.0
    order.apply_fill(60.0, 20.0, 2000);
    EXPECT_DOUBLE_EQ(order.cum_qty, 100.0);
    EXPECT_DOUBLE_EQ(order.leaves_qty, 0.0);
    EXPECT_DOUBLE_EQ(order.avg_px, 16.0);
    EXPECT_EQ(order.updated_at_mono_ns, 2000);
    EXPECT_TRUE(order.check_quantity_invariant());
}
