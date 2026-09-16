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
using InboundOrderBuffer = core::SpscRingBuffer<wire::OrderEventWire, kDefaultGatewayQueueCapacity>;
using InboundCancelBuffer = core::SpscRingBuffer<wire::CancelEventWire, kDefaultGatewayQueueCapacity>;
using OutboundExecBuffer = core::SpscRingBuffer<wire::ExecReportEventWire, kDefaultGatewayQueueCapacity>;

class InboundGatewayApplication : public FIX::Application {
public:
    InboundGatewayApplication(InboundOrderBuffer& order_buf,
                              InboundCancelBuffer& cancel_buf,
                              OutboundExecBuffer& exec_buf) noexcept;

    ~InboundGatewayApplication() override = default;

    // QuickFIX Application interface
    void onCreate(const FIX::SessionID& sessionID) override;
    void onLogon(const FIX::SessionID& sessionID) override;
    void onLogout(const FIX::SessionID& sessionID) override;
    void toAdmin(FIX::Message& msg, const FIX::SessionID& sessionID) override;
    void toApp(FIX::Message& msg, const FIX::SessionID& sessionID) override;
    void fromAdmin(const FIX::Message& msg, const FIX::SessionID& sessionID) override;
    void fromApp(const FIX::Message& msg, const FIX::SessionID& sessionID) override;

    // Outbound drain: Translates ExecReportEventWire -> FIX 35=8 and sends to client
    size_t drain_outbound(const FIX::SessionID& sessionID);

    // Monitoring and status queries
    [[nodiscard]] bool is_logged_on() const noexcept { return is_logged_on_.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t orders_received() const noexcept { return orders_received_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t cancels_received() const noexcept { return cancels_received_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t validation_errors() const noexcept { return validation_errors_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t reports_sent() const noexcept { return reports_sent_.load(std::memory_order_relaxed); }

    // Helpers
    static uint64_t current_mono_ns() noexcept;

private:
    InboundOrderBuffer& order_buffer_;
    InboundCancelBuffer& cancel_buffer_;
    OutboundExecBuffer& exec_buffer_;

    std::atomic<bool> is_logged_on_{false};
    std::atomic<uint64_t> orders_received_{0};
    std::atomic<uint64_t> cancels_received_{0};
    std::atomic<uint64_t> validation_errors_{0};
    std::atomic<uint64_t> reports_sent_{0};

    bool parse_new_order_single(const FIX::Message& msg, uint32_t session_num, uint64_t mono_ts_ns);
    bool parse_order_cancel_request(const FIX::Message& msg, uint32_t session_num, uint64_t mono_ts_ns);
};

} // namespace kestrel::gateway
