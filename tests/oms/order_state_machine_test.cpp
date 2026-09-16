#include <gtest/gtest.h>
#include <iostream>
#include "kestrel/oms/order_state_machine.h"

using namespace kestrel::oms;

TEST(OrderStateMachineTest, TableDimensionsAndConstexpr) {
    static_assert(kTransitionTable.size() == static_cast<size_t>(OrderState::Count));
    static_assert(kTransitionTable[0].size() == static_cast<size_t>(OrderEvent::Count));
    EXPECT_EQ(kTransitionTable.size(), 11);
    EXPECT_EQ(kTransitionTable[0].size(), 9);
}

TEST(OrderStateMachineTest, LegalTransitions) {
    // PendingNew
    EXPECT_EQ(transition(OrderState::PendingNew, OrderEvent::Ack), OrderState::New);
    EXPECT_TRUE(is_legal_transition(OrderState::PendingNew, OrderEvent::Ack));

    EXPECT_EQ(transition(OrderState::PendingNew, OrderEvent::RejectEvent), OrderState::Rejected);
    EXPECT_TRUE(is_legal_transition(OrderState::PendingNew, OrderEvent::RejectEvent));

    // New
    EXPECT_EQ(transition(OrderState::New, OrderEvent::PartialFill), OrderState::PartiallyFilled);
    EXPECT_EQ(transition(OrderState::New, OrderEvent::FullFill), OrderState::Filled);
    EXPECT_EQ(transition(OrderState::New, OrderEvent::CancelRequest), OrderState::PendingCancel);
    EXPECT_EQ(transition(OrderState::New, OrderEvent::ReplaceRequest), OrderState::PendingReplace);
    EXPECT_EQ(transition(OrderState::New, OrderEvent::RejectEvent), OrderState::Rejected);
    EXPECT_EQ(transition(OrderState::New, OrderEvent::ExpireEvent), OrderState::Expired);

    // PartiallyFilled
    EXPECT_EQ(transition(OrderState::PartiallyFilled, OrderEvent::PartialFill), OrderState::PartiallyFilled);
    EXPECT_EQ(transition(OrderState::PartiallyFilled, OrderEvent::FullFill), OrderState::Filled);
    EXPECT_EQ(transition(OrderState::PartiallyFilled, OrderEvent::CancelRequest), OrderState::PendingCancel);

    // PendingCancel
    EXPECT_EQ(transition(OrderState::PendingCancel, OrderEvent::CancelAck), OrderState::Cancelled);
    EXPECT_EQ(transition(OrderState::PendingCancel, OrderEvent::RejectEvent), OrderState::New);
    EXPECT_EQ(transition(OrderState::PendingCancel, OrderEvent::PartialFill), OrderState::PendingCancel);
    EXPECT_EQ(transition(OrderState::PendingCancel, OrderEvent::FullFill), OrderState::Filled);

    // PendingReplace
    EXPECT_EQ(transition(OrderState::PendingReplace, OrderEvent::ReplaceAck), OrderState::Replaced);
    EXPECT_EQ(transition(OrderState::PendingReplace, OrderEvent::RejectEvent), OrderState::New);
}

TEST(OrderStateMachineTest, UndefinedTransitionsDefaultToRejected) {
    EXPECT_EQ(transition(OrderState::Filled, OrderEvent::CancelRequest), OrderState::Rejected);
    EXPECT_FALSE(is_legal_transition(OrderState::Filled, OrderEvent::CancelRequest));

    EXPECT_EQ(transition(OrderState::Cancelled, OrderEvent::FullFill), OrderState::Rejected);
    EXPECT_FALSE(is_legal_transition(OrderState::Cancelled, OrderEvent::FullFill));

    EXPECT_EQ(transition(OrderState::PendingNew, OrderEvent::CancelRequest), OrderState::Rejected);
    EXPECT_FALSE(is_legal_transition(OrderState::PendingNew, OrderEvent::CancelRequest));
}

TEST(OrderStateMachineTest, ExhaustiveProgrammaticPairEnumeration) {
    constexpr size_t StateCount = static_cast<size_t>(OrderState::Count);
    constexpr size_t EventCount = static_cast<size_t>(OrderEvent::Count);
    constexpr size_t ExpectedTotalPairs = StateCount * EventCount;

    size_t evaluated_pairs = 0;
    size_t legal_count = 0;
    size_t rejected_count = 0;

    for (size_t s = 0; s < StateCount; ++s) {
        const auto state = static_cast<OrderState>(s);
        for (size_t e = 0; e < EventCount; ++e) {
            const auto event = static_cast<OrderEvent>(e);
            const auto next = transition(state, event);
            const bool legal = is_legal_transition(state, event);

            ++evaluated_pairs;

            if (legal) {
                ++legal_count;
                // Every legal transition must transition to a valid state
                EXPECT_NE(next, OrderState::Count);
            } else {
                ++rejected_count;
                // Every non-legal transition must resolve to Rejected
                EXPECT_EQ(next, OrderState::Rejected)
                    << "State " << to_string(state) << " + Event " << to_string(event)
                    << " was not legal, but transitioned to " << to_string(next) << " instead of Rejected";
            }
        }
    }

    EXPECT_EQ(evaluated_pairs, ExpectedTotalPairs);
    EXPECT_EQ(evaluated_pairs, 11 * 9);
    EXPECT_EQ(legal_count, 17);
    EXPECT_EQ(rejected_count, (11 * 9) - 17);
}

TEST(OrderStateMachineTest, TerminalStatesNeverTransition) {
    constexpr size_t StateCount = static_cast<size_t>(OrderState::Count);
    constexpr size_t EventCount = static_cast<size_t>(OrderEvent::Count);

    for (size_t s = 0; s < StateCount; ++s) {
        const auto state = static_cast<OrderState>(s);
        if (is_terminal(state)) {
            for (size_t e = 0; e < EventCount; ++e) {
                const auto event = static_cast<OrderEvent>(e);
                const auto next = transition(state, event);
                const bool legal = is_legal_transition(state, event);

                EXPECT_FALSE(legal)
                    << "Terminal state " << to_string(state) << " must not have legal transition for event " << to_string(event);
                EXPECT_EQ(next, OrderState::Rejected)
                    << "Terminal state " << to_string(state) << " must resolve to Rejected for event " << to_string(event);
            }
        }
    }
}
