#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace kestrel::core {

template <typename T, size_t Capacity>
class SpscRingBuffer {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2 (enables mask instead of modulo)");

    static constexpr uint64_t mask_ = Capacity - 1;

    // Cache line 1: Producer write / Consumer read
    alignas(64) std::atomic<uint64_t> head_{0};
    char pad_head_[64 - sizeof(std::atomic<uint64_t>)]{};

    // Cache line 2: Consumer write / Producer read
    alignas(64) std::atomic<uint64_t> tail_{0};
    char pad_tail_[64 - sizeof(std::atomic<uint64_t>)]{};

    // Buffer storage aligned to cache line
    alignas(64) std::array<T, Capacity> buffer_{};

public:
    SpscRingBuffer() noexcept = default;
    ~SpscRingBuffer() noexcept = default;

    // Non-copyable, non-movable to prevent accidental copy/move of ring buffer
    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;
    SpscRingBuffer(SpscRingBuffer&&) = delete;
    SpscRingBuffer& operator=(SpscRingBuffer&&) = delete;

    [[nodiscard]] bool try_push(const T& item) noexcept {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= Capacity) {
            return false; // buffer full
        }
        buffer_[h & mask_] = item;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_push(T&& item) noexcept {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= Capacity) {
            return false; // buffer full
        }
        buffer_[h & mask_] = std::move(item);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out) noexcept {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        const uint64_t h = head_.load(std::memory_order_acquire);
        if (t == h) {
            return false; // buffer empty
        }
        out = std::move(buffer_[t & mask_]);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] size_t size_approx() const noexcept {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        return (h >= t) ? static_cast<size_t>(h - t) : 0;
    }

    [[nodiscard]] bool empty_approx() const noexcept {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept {
        return Capacity;
    }
};

} // namespace kestrel::core
