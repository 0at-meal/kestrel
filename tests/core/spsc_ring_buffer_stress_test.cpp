#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <cstdint>
#include "kestrel/core/spsc_ring_buffer.h"
#include "kestrel/wire/wire_structs.h"

using namespace kestrel::core;
using namespace kestrel::wire;

TEST(SpscRingBufferStressTest, ConcurrentProducerConsumerOneMillionMessages) {
    constexpr size_t Capacity = 1024;
    constexpr uint64_t MessageCount = 1'000'000;

    auto ring = std::make_unique<SpscRingBuffer<uint64_t, Capacity>>();

    std::atomic<bool> start_flag{false};

    std::thread producer([&]() {
        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (uint64_t i = 0; i < MessageCount; ++i) {
            while (!ring->try_push(i)) {
                // Exercise backpressure: spin or yield when full
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&]() {
        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        uint64_t expected = 0;
        while (expected < MessageCount) {
            uint64_t val = 0;
            if (ring->try_pop(val)) {
                EXPECT_EQ(val, expected);
                ++expected;
            } else {
                // Exercise empty drain: yield when empty
                std::this_thread::yield();
            }
        }
    });

    start_flag.store(true, std::memory_order_release);

    producer.join();
    consumer.join();

    EXPECT_EQ(ring->size_approx(), 0);
}

TEST(SpscRingBufferStressTest, ConcurrentWireStructStress) {
    constexpr size_t Capacity = 512;
    constexpr uint64_t MessageCount = 200'000;

    auto ring = std::make_unique<SpscRingBuffer<OrderEventWire, Capacity>>();

    std::atomic<bool> start_flag{false};

    std::thread producer([&]() {
        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (uint64_t i = 0; i < MessageCount; ++i) {
            OrderEventWire order{};
            order.price = static_cast<double>(i);
            order.quantity = 10.0;
            order.session_id = static_cast<uint32_t>(i % 16);
            order.mono_ts_ns = i;

            while (!ring->try_push(order)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&]() {
        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        uint64_t expected = 0;
        while (expected < MessageCount) {
            OrderEventWire order{};
            if (ring->try_pop(order)) {
                EXPECT_DOUBLE_EQ(order.price, static_cast<double>(expected));
                EXPECT_DOUBLE_EQ(order.quantity, 10.0);
                EXPECT_EQ(order.session_id, static_cast<uint32_t>(expected % 16));
                EXPECT_EQ(order.mono_ts_ns, expected);
                ++expected;
            } else {
                std::this_thread::yield();
            }
        }
    });

    start_flag.store(true, std::memory_order_release);

    producer.join();
    consumer.join();

    EXPECT_EQ(ring->size_approx(), 0);
}
