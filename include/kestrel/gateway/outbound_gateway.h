#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <quickfix/Application.h>
#include <quickfix/Message.h>
#include <quickfix/Session.h>
#include <quickfix/SessionID.h>
#include <quickfix/Values.h>
#include <quickfix/fix44/ExecutionReport.h>
#include <quickfix/fix44/NewOrderSingle.h>
#include <quickfix/fix44/OrderCancelRequest.h>

#include "kestrel/core/spsc_ring_buffer.h"
#include "kestrel/wire/enums.h"
#include "kestrel/wire/wire_structs.h"

namespace kestrel::gateway {

constexpr size_t kDefaultGatewayQueueCapacity = 1024;
using OutboundOrderBuffer = core::SpscRingBuffer<wire::OrderEventWire, kDefaultGatewayQueueCapacity>;
using OutboundCancelBuffer = core::SpscRingBuffer<wire::CancelEventWire, kDefaultGatewayQueueCapacity>;
using InboundExecBuffer = core::SpscRingBuffer<wire::ExecReportEventWire, kDefaultGatewayQueueCapacity>;

class OutboundGatewayApplication : public FIX::Application {
public:
    OutboundGatewayApplication(OutboundOrderBuffer& order_buf,
                               OutboundCancelBuffer& cancel_buf,
                               InboundExecBuffer& exec_buf) noexcept;

    ~OutboundGatewayApplication() override = default;

    // QuickFIX Application interface
    void onCreate(const FIX::SessionID& sessionID) override;
    void onLogon(const FIX::SessionID& sessionID) override;
    void onLogout(const FIX::SessionID& sessionID) override;
    void toAdmin(FIX::Message& msg, const FIX::SessionID& sessionID) override;
    void toApp(FIX::Message& msg, const FIX::SessionID& sessionID) override;
    void fromAdmin(const FIX::Message& msg, const FIX::SessionID& sessionID) override;
    void fromApp(const FIX::Message& msg, const FIX::SessionID& sessionID) override;

    // Send side: Drains OMS Core outbound routing buffers and sends FIX orders/cancels to venue
    size_t drain_outbound(const FIX::SessionID& venue_session);

    // Monitoring and status queries
    [[nodiscard]] bool is_logged_on() const noexcept { return is_logged_on_.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t orders_sent() const noexcept { return orders_sent_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t cancels_sent() const noexcept { return cancels_sent_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t reports_received() const noexcept { return reports_received_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t validation_errors() const noexcept { return validation_errors_.load(std::memory_order_relaxed); }

    // Helpers
    static uint64_t current_mono_ns() noexcept;
    static uint64_t extract_order_id(const FIX::Message& msg) noexcept;

private:
    OutboundOrderBuffer& order_buffer_;
    OutboundCancelBuffer& cancel_buffer_;
    InboundExecBuffer& exec_buffer_;

    std::atomic<bool> is_logged_on_{false};
    std::atomic<uint64_t> orders_sent_{0};
    std::atomic<uint64_t> cancels_sent_{0};
    std::atomic<uint64_t> reports_received_{0};
    std::atomic<uint64_t> validation_errors_{0};

    bool parse_execution_report(const FIX::Message& msg, uint64_t mono_ts_ns);
};

} // namespace kestrel::gateway
