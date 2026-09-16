#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "kestrel/oms/order.h"

namespace kestrel::oms {

class OrderPool {
public:
    explicit OrderPool(size_t capacity);
    ~OrderPool() = default;

    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete;
    OrderPool(OrderPool&&) noexcept = default;
    OrderPool& operator=(OrderPool&&) noexcept = default;

    // Allocates next sequential order slot. Returns nullopt if exhausted.
    [[nodiscard]] std::optional<uint64_t> allocate();

    // O(1) index access to Order
    [[nodiscard]] Order& get(uint64_t order_id);
    [[nodiscard]] const Order& get(uint64_t order_id) const;

    // ClOrdID mapping and duplicate detection (FR-014)
    [[nodiscard]] bool register_cl_ord_id(std::string_view cl_ord_id, uint64_t order_id);
    [[nodiscard]] std::optional<uint64_t> find_by_cl_ord_id(std::string_view cl_ord_id) const;

    [[nodiscard]] size_t size() const noexcept { return next_free_; }
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool is_full() const noexcept { return next_free_ >= capacity_; }

private:
    size_t capacity_{0};
    size_t next_free_{0};
    std::vector<Order> slots_;
    std::unordered_map<std::string, uint64_t> cl_ord_id_map_;
};

} // namespace kestrel::oms
