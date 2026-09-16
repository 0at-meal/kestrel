#include <gtest/gtest.h>
#include "kestrel/core/spsc_ring_buffer.h"
#include "kestrel/wire/wire_structs.h"

using namespace kestrel::core;
using namespace kestrel::wire;

TEST(SpscRingBufferTest, AlignmentAndOffsets) {
    using BufferType = SpscRingBuffer<int, 64>;
    // Check that buffer can be instantiated and sizeof is reasonable
    EXPECT_GE(sizeof(BufferType), 128);

    BufferType ring{};
    EXPECT_EQ(ring.capacity(), 64);
    EXPECT_EQ(ring.size_approx(), 0);
    EXPECT_TRUE(ring.empty_approx());
}

TEST(SpscRingBufferTest, EmptyPop) {
    SpscRingBuffer<int, 4> ring{};
    int val = 999;
    EXPECT_FALSE(ring.try_pop(val));
    EXPECT_EQ(val, 999); // unchanged
    EXPECT_EQ(ring.size_approx(), 0);
}

TEST(SpscRingBufferTest, SingleItemPushPop) {
    SpscRingBuffer<int, 4> ring{};
    EXPECT_TRUE(ring.try_push(42));
    EXPECT_EQ(ring.size_approx(), 1);

    int out = 0;
    EXPECT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, 42);
    EXPECT_EQ(ring.size_approx(), 0);
    EXPECT_FALSE(ring.try_pop(out));
}

TEST(SpscRingBufferTest, FullCapacityPushAndBackpressure) {
    SpscRingBuffer<int, 4> ring{};
    EXPECT_TRUE(ring.try_push(1));
    EXPECT_TRUE(ring.try_push(2));
    EXPECT_TRUE(ring.try_push(3));
    EXPECT_TRUE(ring.try_push(4));

    // Now full
    EXPECT_FALSE(ring.try_push(5));
    EXPECT_EQ(ring.size_approx(), 4);

    int out = 0;
    EXPECT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, 1);
    EXPECT_EQ(ring.size_approx(), 3);

    // Can push one more now
    EXPECT_TRUE(ring.try_push(5));
    EXPECT_FALSE(ring.try_push(6));

    EXPECT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, 2);
    EXPECT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, 3);
    EXPECT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, 4);
    EXPECT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, 5);
    EXPECT_FALSE(ring.try_pop(out));
}

TEST(SpscRingBufferTest, WraparoundCorrectness) {
    constexpr size_t Cap = 8;
    SpscRingBuffer<uint64_t, Cap> ring{};

    constexpr uint64_t TotalIterations = 10000;
    for (uint64_t i = 0; i < TotalIterations; ++i) {
        EXPECT_TRUE(ring.try_push(i));
        uint64_t out = 0;
        EXPECT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_EQ(ring.size_approx(), 0);
}

TEST(SpscRingBufferTest, WireStructHandling) {
    SpscRingBuffer<OrderEventWire, 16> ring{};

    OrderEventWire order{};
    order.price = 100.50;
    order.quantity = 10.0;
    order.session_id = 1;
    order.mono_ts_ns = 555;

    EXPECT_TRUE(ring.try_push(order));

    OrderEventWire received{};
    EXPECT_TRUE(ring.try_pop(received));
    EXPECT_DOUBLE_EQ(received.price, 100.50);
    EXPECT_DOUBLE_EQ(received.quantity, 10.0);
    EXPECT_EQ(received.session_id, 1);
    EXPECT_EQ(received.mono_ts_ns, 555);
}
