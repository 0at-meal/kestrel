#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <quickfix/Application.h>
#include <quickfix/Message.h>
#include <quickfix/Session.h>
#include <quickfix/SessionID.h>
#include <quickfix/Values.h>
#include <quickfix/fix44/ExecutionReport.h>
#include <quickfix/fix44/NewOrderSingle.h>
#include <quickfix/fix44/OrderCancelRequest.h>

namespace kestrel::venue {

class StubVenueApplication : public FIX::Application {
public:
    StubVenueApplication() noexcept = default;
    ~StubVenueApplication() override = default;

    // QuickFIX Application callbacks
    void onCreate(const FIX::SessionID& sessionID) override;
    void onLogon(const FIX::SessionID& sessionID) override;
    void onLogout(const FIX::SessionID& sessionID) override;
    void toAdmin(FIX::Message& msg, const FIX::SessionID& sessionID) override;
    void toApp(FIX::Message& msg, const FIX::SessionID& sessionID) override;
    void fromAdmin(const FIX::Message& msg, const FIX::SessionID& sessionID) override;
    void fromApp(const FIX::Message& msg, const FIX::SessionID& sessionID) override;

    [[nodiscard]] bool is_logged_on() const noexcept { return is_logged_on_.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t orders_received() const noexcept { return orders_received_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t cancels_received() const noexcept { return cancels_received_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t reports_sent() const noexcept { return reports_sent_.load(std::memory_order_relaxed); }

    // For unit testing: access generated execution reports
    [[nodiscard]] const std::vector<FIX44::ExecutionReport>& generated_reports() const noexcept {
        return generated_reports_;
    }
    void clear_generated_reports() noexcept {
        generated_reports_.clear();
    }

private:
    std::atomic<bool> is_logged_on_{false};
    std::atomic<uint64_t> orders_received_{0};
    std::atomic<uint64_t> cancels_received_{0};
    std::atomic<uint64_t> reports_sent_{0};
    uint64_t order_seq_{0};
    uint64_t exec_seq_{0};

    std::vector<FIX44::ExecutionReport> generated_reports_;

    void handle_new_order_single(const FIX::Message& msg, const FIX::SessionID& sessionID);
    void handle_order_cancel_request(const FIX::Message& msg, const FIX::SessionID& sessionID);
    void send_or_record_report(FIX44::ExecutionReport report, const FIX::SessionID& sessionID);
};

} // namespace kestrel::venue
