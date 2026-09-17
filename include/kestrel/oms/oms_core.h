#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>
#include <thread>
#include "kestrel/core/spsc_ring_buffer.h"
#include "kestrel/gateway/inbound_gateway.h"
#include "kestrel/gateway/outbound_gateway.h"
#include "kestrel/oms/order.h"
#include "kestrel/oms/order_pool.h"
#include "kestrel/oms/order_state.h"
#include "kestrel/oms/order_state_machine.h"
#include "kestrel/perf/latency_histogram.h"
#include "kestrel/wire/enums.h"
#include "kestrel/wire/wire_structs.h"

namespace kestrel::oms {

class OmsCore {
public:
    OmsCore(size_t order_pool_capacity,
            gateway::InboundOrderBuffer& inbound_orders,
            gateway::InboundCancelBuffer& inbound_cancels,
            gateway::OutboundExecBuffer& client_exec_reports,
            gateway::OutboundOrderBuffer& outbound_orders,
            gateway::OutboundCancelBuffer& outbound_cancels,
            gateway::InboundExecBuffer& venue_exec_reports) noexcept;

    ~OmsCore();

    OmsCore(const OmsCore&) = delete;
    OmsCore& operator=(const OmsCore&) = delete;
    OmsCore(OmsCore&&) = delete;
    OmsCore& operator=(OmsCore&&) = delete;

    // Single-threaded poll methods (for deterministic step testing and pipeline loop)
    size_t poll_once() noexcept;
    size_t poll_all() noexcept;

    // Background thread lifecycle
    void start();
    void stop();
    [[nodiscard]] bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }

    // Statistics and monitoring
    [[nodiscard]] uint64_t orders_processed() const noexcept { return orders_processed_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t cancels_processed() const noexcept { return cancels_processed_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t reports_processed() const noexcept { return reports_processed_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t orders_rejected() const noexcept { return orders_rejected_.load(std::memory_order_relaxed); }

    [[nodiscard]] const perf::LatencyHistogram& latency_histogram() const noexcept { return latency_histogram_; }
    [[nodiscard]] OrderPool& order_pool() noexcept { return order_pool_; }
    [[nodiscard]] const OrderPool& order_pool() const noexcept { return order_pool_; }

    static uint64_t current_mono_ns() noexcept;
    static wire::OrdStatus to_wire_ord_status(OrderState state) noexcept;

private:
    OrderPool order_pool_;

    gateway::InboundOrderBuffer& inbound_orders_;
    gateway::InboundCancelBuffer& inbound_cancels_;
    gateway::OutboundExecBuffer& client_exec_reports_;

    gateway::OutboundOrderBuffer& outbound_orders_;
    gateway::OutboundCancelBuffer& outbound_cancels_;
    gateway::InboundExecBuffer& venue_exec_reports_;

    perf::LatencyHistogram latency_histogram_;

    std::atomic<bool> running_{false};
    std::jthread worker_thread_;

    std::atomic<uint64_t> orders_processed_{0};
    std::atomic<uint64_t> cancels_processed_{0};
    std::atomic<uint64_t> reports_processed_{0};
    std::atomic<uint64_t> orders_rejected_{0};

    uint64_t exec_id_seq_{0};

    void process_inbound_order(const wire::OrderEventWire& wire) noexcept;
    void process_inbound_cancel(const wire::CancelEventWire& wire) noexcept;
    void process_venue_exec_report(const wire::ExecReportEventWire& wire) noexcept;

    void emit_client_exec_report(
        uint64_t order_id,
        wire::ExecType exec_type,
        wire::OrdStatus ord_status,
        double leaves_qty,
        double cum_qty,
        double last_qty,
        double last_px,
        uint64_t mono_ts_ns) noexcept;
};

} // namespace kestrel::oms
