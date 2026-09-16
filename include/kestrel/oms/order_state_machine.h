#pragma once

#include <array>
#include <cstddef>
#include "kestrel/oms/order_state.h"

namespace kestrel::oms {

namespace detail {

constexpr auto init_transition_tables() {
    struct Tables {
        std::array<std::array<OrderState, static_cast<size_t>(OrderEvent::Count)>,
                   static_cast<size_t>(OrderState::Count)> next_state{};
        std::array<std::array<bool, static_cast<size_t>(OrderEvent::Count)>,
                   static_cast<size_t>(OrderState::Count)> is_legal{};
    };

    Tables t{};

    // Default every cell to Rejected and not legal
    for (size_t s = 0; s < static_cast<size_t>(OrderState::Count); ++s) {
        for (size_t e = 0; e < static_cast<size_t>(OrderEvent::Count); ++e) {
            t.next_state[s][e] = OrderState::Rejected;
            t.is_legal[s][e] = false;
        }
    }

    auto set_legal = [&t](OrderState from, OrderEvent ev, OrderState to) {
        t.next_state[static_cast<size_t>(from)][static_cast<size_t>(ev)] = to;
        t.is_legal[static_cast<size_t>(from)][static_cast<size_t>(ev)] = true;
    };

    // Explicit legal transitions per PRD §6.2 / LOW_LEVEL_DESIGN §2 / P1-T06
    set_legal(OrderState::PendingNew,     OrderEvent::Ack,            OrderState::New);
    set_legal(OrderState::PendingNew,     OrderEvent::RejectEvent,    OrderState::Rejected);

    set_legal(OrderState::New,            OrderEvent::PartialFill,    OrderState::PartiallyFilled);
    set_legal(OrderState::New,            OrderEvent::FullFill,       OrderState::Filled);
    set_legal(OrderState::New,            OrderEvent::CancelRequest,  OrderState::PendingCancel);
    set_legal(OrderState::New,            OrderEvent::ReplaceRequest, OrderState::PendingReplace);
    set_legal(OrderState::New,            OrderEvent::RejectEvent,    OrderState::Rejected);
    set_legal(OrderState::New,            OrderEvent::ExpireEvent,    OrderState::Expired);

    set_legal(OrderState::PartiallyFilled, OrderEvent::PartialFill,   OrderState::PartiallyFilled);
    set_legal(OrderState::PartiallyFilled, OrderEvent::FullFill,      OrderState::Filled);
    set_legal(OrderState::PartiallyFilled, OrderEvent::CancelRequest, OrderState::PendingCancel);

    set_legal(OrderState::PendingCancel,  OrderEvent::CancelAck,      OrderState::Cancelled);
    set_legal(OrderState::PendingCancel,  OrderEvent::RejectEvent,    OrderState::New); // Cancel rejected, revert
    set_legal(OrderState::PendingCancel,  OrderEvent::PartialFill,    OrderState::PendingCancel); // Fill while pending cancel
    set_legal(OrderState::PendingCancel,  OrderEvent::FullFill,       OrderState::Filled); // Filled before cancel processed

    set_legal(OrderState::PendingReplace, OrderEvent::ReplaceAck,     OrderState::Replaced);
    set_legal(OrderState::PendingReplace, OrderEvent::RejectEvent,    OrderState::New); // Replace rejected, revert

    return t;
}

inline constexpr auto kTransitionData = init_transition_tables();

} // namespace detail

inline constexpr const auto& kTransitionTable = detail::kTransitionData.next_state;

[[nodiscard]] constexpr OrderState transition(OrderState current, OrderEvent event) noexcept {
    const auto s = static_cast<size_t>(current);
    const auto e = static_cast<size_t>(event);
    if (s >= static_cast<size_t>(OrderState::Count) || e >= static_cast<size_t>(OrderEvent::Count)) {
        return OrderState::Rejected;
    }
    return detail::kTransitionData.next_state[s][e];
}

[[nodiscard]] constexpr bool is_legal_transition(OrderState current, OrderEvent event) noexcept {
    const auto s = static_cast<size_t>(current);
    const auto e = static_cast<size_t>(event);
    if (s >= static_cast<size_t>(OrderState::Count) || e >= static_cast<size_t>(OrderEvent::Count)) {
        return false;
    }
    return detail::kTransitionData.is_legal[s][e];
}

} // namespace kestrel::oms
