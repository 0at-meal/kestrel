#pragma once

#include <cstdint>
#include <string_view>

namespace kestrel::oms {

enum class OrderState : uint8_t {
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

[[nodiscard]] constexpr std::string_view to_string(OrderState state) noexcept {
    switch (state) {
        case OrderState::PendingNew:     return "PendingNew";
        case OrderState::New:            return "New";
        case OrderState::PartiallyFilled:return "PartiallyFilled";
        case OrderState::Filled:         return "Filled";
        case OrderState::PendingCancel:  return "PendingCancel";
        case OrderState::Cancelled:      return "Cancelled";
        case OrderState::PendingReplace: return "PendingReplace";
        case OrderState::Replaced:       return "Replaced";
        case OrderState::Rejected:       return "Rejected";
        case OrderState::Expired:        return "Expired";
        case OrderState::DoneForDay:     return "DoneForDay";
        case OrderState::Count:          return "Count";
    }
    return "Unknown";
}

[[nodiscard]] constexpr bool is_terminal(OrderState state) noexcept {
    switch (state) {
        case OrderState::Filled:
        case OrderState::Cancelled:
        case OrderState::Replaced:
        case OrderState::Rejected:
        case OrderState::Expired:
        case OrderState::DoneForDay:
            return true;
        case OrderState::PendingNew:
        case OrderState::New:
        case OrderState::PartiallyFilled:
        case OrderState::PendingCancel:
        case OrderState::PendingReplace:
        case OrderState::Count:
            return false;
    }
    return false;
}

enum class OrderEvent : uint8_t {
    Ack = 0,
    PartialFill = 1,
    FullFill = 2,
    CancelRequest = 3,
    CancelAck = 4,
    ReplaceRequest = 5,
    ReplaceAck = 6,
    RejectEvent = 7,
    ExpireEvent = 8,
    Count = 9
};

[[nodiscard]] constexpr std::string_view to_string(OrderEvent event) noexcept {
    switch (event) {
        case OrderEvent::Ack:            return "Ack";
        case OrderEvent::PartialFill:    return "PartialFill";
        case OrderEvent::FullFill:       return "FullFill";
        case OrderEvent::CancelRequest:  return "CancelRequest";
        case OrderEvent::CancelAck:      return "CancelAck";
        case OrderEvent::ReplaceRequest: return "ReplaceRequest";
        case OrderEvent::ReplaceAck:     return "ReplaceAck";
        case OrderEvent::RejectEvent:    return "RejectEvent";
        case OrderEvent::ExpireEvent:    return "ExpireEvent";
        case OrderEvent::Count:          return "Count";
    }
    return "Unknown";
}

} // namespace kestrel::oms
