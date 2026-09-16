#include "kestrel/oms/order_pool.h"
#include <stdexcept>

namespace kestrel::oms {

OrderPool::OrderPool(size_t capacity)
    : capacity_(capacity), next_free_(0) {
    slots_.resize(capacity);
    cl_ord_id_map_.reserve(capacity);
}

std::optional<uint64_t> OrderPool::allocate() {
    if (next_free_ >= capacity_) {
        return std::nullopt;
    }
    const uint64_t id = next_free_++;
    slots_[id] = Order{};
    slots_[id].order_id = id;
    return id;
}

Order& OrderPool::get(uint64_t order_id) {
    return slots_[order_id];
}

const Order& OrderPool::get(uint64_t order_id) const {
    return slots_[order_id];
}

bool OrderPool::register_cl_ord_id(std::string_view cl_ord_id, uint64_t order_id) {
    std::string key(cl_ord_id);
    auto [_, inserted] = cl_ord_id_map_.try_emplace(std::move(key), order_id);
    return inserted;
}

std::optional<uint64_t> OrderPool::find_by_cl_ord_id(std::string_view cl_ord_id) const {
    auto it = cl_ord_id_map_.find(std::string(cl_ord_id));
    if (it != cl_ord_id_map_.end()) {
        return it->second;
    }
    return std::nullopt;
}

} // namespace kestrel::oms
