#include "kestrel/perf/latency_histogram.h"
#include <algorithm>
#include <cmath>

namespace kestrel::perf {

LatencyHistogram::LatencyHistogram() noexcept {
    reset();
}

void LatencyHistogram::reset() noexcept {
    buckets_.fill(0);
    total_count_ = 0;
    min_ns_ = std::numeric_limits<uint64_t>::max();
    max_ns_ = 0;
}

uint64_t LatencyHistogram::percentile(double p) const noexcept {
    if (total_count_ == 0) {
        return 0;
    }
    if (p <= 0.0) {
        return min_ns_;
    }
    if (p >= 100.0) {
        return max_ns_;
    }

    const auto rank = static_cast<uint64_t>(std::round((p / 100.0) * static_cast<double>(total_count_)));
    const uint64_t target_count = std::clamp(rank, static_cast<uint64_t>(1), total_count_);
    uint64_t accumulated = 0;

    for (size_t i = 0; i < kBucketCount; ++i) {
        accumulated += buckets_[i];
        if (accumulated >= target_count) {
            uint64_t val = bucket_to_value(i);
            if (val < min_ns_) val = min_ns_;
            if (val > max_ns_) val = max_ns_;
            return val;
        }
    }

    return max_ns_;
}

void LatencyHistogram::merge(const LatencyHistogram& other) noexcept {
    if (other.total_count_ == 0) {
        return;
    }
    for (size_t i = 0; i < kBucketCount; ++i) {
        buckets_[i] += other.buckets_[i];
    }
    total_count_ += other.total_count_;
    if (other.min_ns_ < min_ns_) min_ns_ = other.min_ns_;
    if (other.max_ns_ > max_ns_) max_ns_ = other.max_ns_;
}

} // namespace kestrel::perf
