#include <gtest/gtest.h>
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
    // Example illegal transitions
    EXPECT_EQ(transition(OrderState::Filled, OrderEvent::CancelRequest), OrderState::Rejected);
    EXPECT_FALSE(is_legal_transition(OrderState::Filled, OrderEvent::CancelRequest));

    EXPECT_EQ(transition(OrderState::Cancelled, OrderEvent::FullFill), OrderState::Rejected);
    EXPECT_FALSE(is_legal_transition(OrderState::Cancelled, OrderEvent::FullFill));

    EXPECT_EQ(transition(OrderState::PendingNew, OrderEvent::CancelRequest), OrderState::Rejected);
    EXPECT_FALSE(is_legal_transition(OrderState::PendingNew, OrderEvent::CancelRequest));
}
