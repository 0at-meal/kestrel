#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace kestrel::perf {

class LatencyHistogram {
public:
    static constexpr size_t kLinearCutoff = 1024;
    static constexpr size_t kSubBucketBits = 6;
    static constexpr size_t kSubBuckets = 1 << kSubBucketBits; // 64
    static constexpr size_t kSubBucketMask = kSubBuckets - 1;  // 0x3F
    static constexpr size_t kMaxBitWidth = 64;
    static constexpr size_t kOctaves = kMaxBitWidth - 10; // from 10 to 63 = 54
    static constexpr size_t kBucketCount = kLinearCutoff + kOctaves * kSubBuckets; // 1024 + 54 * 64 = 4480

    LatencyHistogram() noexcept;
    ~LatencyHistogram() noexcept = default;

    LatencyHistogram(const LatencyHistogram&) = default;
    LatencyHistogram& operator=(const LatencyHistogram&) = default;
    LatencyHistogram(LatencyHistogram&&) noexcept = default;
    LatencyHistogram& operator=(LatencyHistogram&&) noexcept = default;

    // Hot-path sample recording: O(1), no locks, no allocation, no exceptions
    void record(uint64_t latency_ns) noexcept {
        const size_t b = compute_bucket(latency_ns);
        ++buckets_[b];
        ++total_count_;
        if (latency_ns < min_ns_) min_ns_ = latency_ns;
        if (latency_ns > max_ns_) max_ns_ = latency_ns;
    }

    // Cold-path percentile and summary reporting
    [[nodiscard]] uint64_t percentile(double p) const noexcept;
    [[nodiscard]] uint64_t p50() const noexcept { return percentile(50.0); }
    [[nodiscard]] uint64_t p99() const noexcept { return percentile(99.0); }
    [[nodiscard]] uint64_t p99_9() const noexcept { return percentile(99.9); }
    [[nodiscard]] uint64_t p99_99() const noexcept { return percentile(99.99); }

    [[nodiscard]] uint64_t min() const noexcept { return (total_count_ > 0) ? min_ns_ : 0; }
    [[nodiscard]] uint64_t max() const noexcept { return (total_count_ > 0) ? max_ns_ : 0; }
    [[nodiscard]] uint64_t total_count() const noexcept { return total_count_; }

    void reset() noexcept;
    void merge(const LatencyHistogram& other) noexcept;

    // Static bucket computation helpers
    [[nodiscard]] static constexpr size_t compute_bucket(uint64_t val_ns) noexcept {
        if (val_ns < kLinearCutoff) {
            return static_cast<size_t>(val_ns);
        }
        const unsigned int w = std::bit_width(val_ns) - 1; // 10 <= w <= 63
        const unsigned int shift = (w >= kSubBucketBits) ? (w - kSubBucketBits) : 0;
        const unsigned int sub = static_cast<unsigned int>((val_ns >> shift) & kSubBucketMask);
        const size_t octave = w - 10;
        const size_t idx = kLinearCutoff + octave * kSubBuckets + sub;
        return (idx < kBucketCount) ? idx : (kBucketCount - 1);
    }

    [[nodiscard]] static constexpr uint64_t bucket_to_value(size_t bucket) noexcept {
        if (bucket < kLinearCutoff) {
            return static_cast<uint64_t>(bucket);
        }
        const size_t idx = bucket - kLinearCutoff;
        const size_t octave = idx / kSubBuckets;
        const size_t sub = idx % kSubBuckets;
        const unsigned int w = static_cast<unsigned int>(octave + 10);
        const uint64_t base = 1ULL << w;
        const uint64_t step = (w >= kSubBucketBits) ? (1ULL << (w - kSubBucketBits)) : 1ULL;
        return base + static_cast<uint64_t>(sub) * step + (step / 2);
    }

private:
    std::array<uint64_t, kBucketCount> buckets_{};
    uint64_t total_count_{0};
    uint64_t min_ns_{std::numeric_limits<uint64_t>::max()};
    uint64_t max_ns_{0};
};

} // namespace kestrel::perf
