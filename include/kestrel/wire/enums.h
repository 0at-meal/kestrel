#pragma once

#include <cstdint>
#include <string_view>

namespace kestrel::wire {

enum class Side : uint8_t {
    Buy = 0,
    Sell = 1,
    Count = 2
};

[[nodiscard]] constexpr std::string_view to_string(Side side) noexcept {
    switch (side) {
        case Side::Buy:   return "Buy";
        case Side::Sell:  return "Sell";
        case Side::Count: return "Count";
    }
    return "Unknown";
}

enum class OrderType : uint8_t {
    Market = 0,
    Limit = 1,
    Count = 2
};

[[nodiscard]] constexpr std::string_view to_string(OrderType type) noexcept {
    switch (type) {
        case OrderType::Market: return "Market";
        case OrderType::Limit:  return "Limit";
        case OrderType::Count:  return "Count";
    }
    return "Unknown";
}

enum class TimeInForce : uint8_t {
    Day = 0,
    GTC = 1,
    Count = 2
};

[[nodiscard]] constexpr std::string_view to_string(TimeInForce tif) noexcept {
    switch (tif) {
        case TimeInForce::Day:   return "Day";
        case TimeInForce::GTC:   return "GTC";
        case TimeInForce::Count: return "Count";
    }
    return "Unknown";
}

enum class ExecType : uint8_t {
    New = 0,
    PartialFill = 1,
    Fill = 2,
    Cancelled = 3,
    Replaced = 4,
    Rejected = 5,
    Expired = 6,
    DoneForDay = 7,
    Count = 8
};

[[nodiscard]] constexpr std::string_view to_string(ExecType type) noexcept {
    switch (type) {
        case ExecType::New:         return "New";
        case ExecType::PartialFill: return "PartialFill";
        case ExecType::Fill:        return "Fill";
        case ExecType::Cancelled:   return "Cancelled";
        case ExecType::Replaced:    return "Replaced";
        case ExecType::Rejected:    return "Rejected";
        case ExecType::Expired:     return "Expired";
        case ExecType::DoneForDay:  return "DoneForDay";
        case ExecType::Count:       return "Count";
    }
    return "Unknown";
}

enum class OrdStatus : uint8_t {
    PendingNew = 0,
    New = 1,
    PartiallyFilled = 2,
    Filled = 3,
    PendingCancel = 4,
    Cancelled = 5,
    PendingReplace = 6,
    Replaced = 7,
    Rejected = 8,
    Expired = 9,
    DoneForDay = 10,
    Count = 11
};

[[nodiscard]] constexpr std::string_view to_string(OrdStatus status) noexcept {
    switch (status) {
        case OrdStatus::PendingNew:     return "PendingNew";
        case OrdStatus::New:            return "New";
        case OrdStatus::PartiallyFilled:return "PartiallyFilled";
        case OrdStatus::Filled:         return "Filled";
        case OrdStatus::PendingCancel:  return "PendingCancel";
        case OrdStatus::Cancelled:      return "Cancelled";
        case OrdStatus::PendingReplace: return "PendingReplace";
        case OrdStatus::Replaced:       return "Replaced";
        case OrdStatus::Rejected:       return "Rejected";
        case OrdStatus::Expired:        return "Expired";
        case OrdStatus::DoneForDay:     return "DoneForDay";
        case OrdStatus::Count:          return "Count";
    }
    return "Unknown";
}

} // namespace kestrel::wire
