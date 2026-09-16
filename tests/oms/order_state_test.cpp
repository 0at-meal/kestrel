#include <gtest/gtest.h>
#include <type_traits>
#include "kestrel/oms/order_state.h"

using namespace kestrel::oms;

TEST(OrderStateTest, UnderlyingTypeAndSentinels) {
    EXPECT_TRUE((std::is_same_v<std::underlying_type_t<OrderState>, uint8_t>));
    EXPECT_TRUE((std::is_same_v<std::underlying_type_t<OrderEvent>, uint8_t>));

    EXPECT_EQ(static_cast<uint8_t>(OrderState::Count), 11);
    EXPECT_EQ(static_cast<uint8_t>(OrderEvent::Count), 9);
}

TEST(OrderStateTest, ToStringCompleteness) {
    for (uint8_t i = 0; i < static_cast<uint8_t>(OrderState::Count); ++i) {
        auto state = static_cast<OrderState>(i);
        auto str = to_string(state);
        EXPECT_NE(str, "Unknown");
        EXPECT_FALSE(str.empty());
    }

    for (uint8_t i = 0; i < static_cast<uint8_t>(OrderEvent::Count); ++i) {
        auto event = static_cast<OrderEvent>(i);
        auto str = to_string(event);
        EXPECT_NE(str, "Unknown");
        EXPECT_FALSE(str.empty());
    }
}

TEST(OrderStateTest, IsTerminalCorrectness) {
    // Non-terminal states
    EXPECT_FALSE(is_terminal(OrderState::PendingNew));
    EXPECT_FALSE(is_terminal(OrderState::New));
    EXPECT_FALSE(is_terminal(OrderState::PartiallyFilled));
    EXPECT_FALSE(is_terminal(OrderState::PendingCancel));
    EXPECT_FALSE(is_terminal(OrderState::PendingReplace));
    EXPECT_FALSE(is_terminal(OrderState::Count));

    // Terminal states
    EXPECT_TRUE(is_terminal(OrderState::Filled));
    EXPECT_TRUE(is_terminal(OrderState::Cancelled));
    EXPECT_TRUE(is_terminal(OrderState::Replaced));
    EXPECT_TRUE(is_terminal(OrderState::Rejected));
    EXPECT_TRUE(is_terminal(OrderState::Expired));
    EXPECT_TRUE(is_terminal(OrderState::DoneForDay));
}
