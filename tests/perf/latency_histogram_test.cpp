#include <gtest/gtest.h>
#include <vector>
#include "kestrel/perf/latency_histogram.h"

using namespace kestrel::perf;

TEST(LatencyHistogramTest, EmptyHistogram) {
    LatencyHistogram hist;
    EXPECT_EQ(hist.total_count(), 0);
    EXPECT_EQ(hist.min(), 0);
    EXPECT_EQ(hist.max(), 0);
    EXPECT_EQ(hist.p50(), 0);
    EXPECT_EQ(hist.p99(), 0);
    EXPECT_EQ(hist.p99_9(), 0);
    EXPECT_EQ(hist.p99_99(), 0);
}

TEST(LatencyHistogramTest, SingleSample) {
    LatencyHistogram hist;
    hist.record(250); // 250 ns
    EXPECT_EQ(hist.total_count(), 1);
    EXPECT_EQ(hist.min(), 250);
    EXPECT_EQ(hist.max(), 250);
    EXPECT_EQ(hist.p50(), 250);
    EXPECT_EQ(hist.p99(), 250);
}

TEST(LatencyHistogramTest, LinearRangeSamples) {
    LatencyHistogram hist;
    // Record 1 to 1000 ns
    for (uint64_t i = 1; i <= 1000; ++i) {
        hist.record(i);
    }
    EXPECT_EQ(hist.total_count(), 1000);
    EXPECT_EQ(hist.min(), 1);
    EXPECT_EQ(hist.max(), 1000);

    // In linear range (<1024), buckets are exact!
    EXPECT_EQ(hist.p50(), 500);
    EXPECT_EQ(hist.p99(), 990);
    EXPECT_EQ(hist.percentile(90.0), 900);
}

TEST(LatencyHistogramTest, HigherRangeAccuracy) {
    LatencyHistogram hist;
    // Record 10,000 samples: 9,000 @ 1,000 ns (1 us), 900 @ 10,000 ns (10 us), 90 @ 100,000 ns (100 us), 10 @ 1,000,000 ns (1 ms)
    for (int i = 0; i < 9000; ++i) hist.record(1000);
    for (int i = 0; i < 900; ++i)  hist.record(10000);
    for (int i = 0; i < 90; ++i)   hist.record(100000);
    for (int i = 0; i < 10; ++i)   hist.record(1000000);

    EXPECT_EQ(hist.total_count(), 10000);

    // p50 should be ~1000 ns (exact since < 1024)
    EXPECT_EQ(hist.p50(), 1000);

    // p90 should be around 1000 to 10000
    EXPECT_LE(hist.percentile(90.0), 10100);

    // p99 should be ~10,000 ns within bucket resolution (< 2% error)
    const uint64_t p99 = hist.p99();
    EXPECT_NEAR(static_cast<double>(p99), 10000.0, 300.0);

    // p99.9 should be ~100,000 ns within bucket resolution
    const uint64_t p99_9 = hist.p99_9();
    EXPECT_NEAR(static_cast<double>(p99_9), 100000.0, 3000.0);

    // p99.99 should be ~1,000,000 ns
    const uint64_t p99_99 = hist.p99_99();
    EXPECT_NEAR(static_cast<double>(p99_99), 1000000.0, 30000.0);
}

TEST(LatencyHistogramTest, MergeAndReset) {
    LatencyHistogram h1;
    LatencyHistogram h2;

    for (int i = 0; i < 500; ++i) h1.record(100);
    for (int i = 0; i < 500; ++i) h2.record(200);

    h1.merge(h2);
    EXPECT_EQ(h1.total_count(), 1000);
    EXPECT_EQ(h1.min(), 100);
    EXPECT_EQ(h1.max(), 200);

    h1.reset();
    EXPECT_EQ(h1.total_count(), 0);
    EXPECT_EQ(h1.min(), 0);
    EXPECT_EQ(h1.max(), 0);
}
