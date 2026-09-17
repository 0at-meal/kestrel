#include "kestrel/oms/oms_core.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <ctime>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace kestrel::oms {

OmsCore::OmsCore(size_t order_pool_capacity,
                 gateway::InboundOrderBuffer& inbound_orders,
                 gateway::InboundCancelBuffer& inbound_cancels,
                 gateway::OutboundExecBuffer& client_exec_reports,
                 gateway::OutboundOrderBuffer& outbound_orders,
                 gateway::OutboundCancelBuffer& outbound_cancels,
                 gateway::InboundExecBuffer& venue_exec_reports) noexcept
    : order_pool_(order_pool_capacity),
      inbound_orders_(inbound_orders),
      inbound_cancels_(inbound_cancels),
      client_exec_reports_(client_exec_reports),
      outbound_orders_(outbound_orders),
      outbound_cancels_(outbound_cancels),
      venue_exec_reports_(venue_exec_reports) {}

OmsCore::~OmsCore() {
    stop();
}

uint64_t OmsCore::current_mono_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

wire::OrdStatus OmsCore::to_wire_ord_status(OrderState state) noexcept {
    switch (state) {
        case OrderState::PendingNew:     return wire::OrdStatus::PendingNew;
        case OrderState::New:            return wire::OrdStatus::New;
        case OrderState::PartiallyFilled:return wire::OrdStatus::PartiallyFilled;
        case OrderState::Filled:         return wire::OrdStatus::Filled;
        case OrderState::PendingCancel:  return wire::OrdStatus::PendingCancel;
        case OrderState::Cancelled:      return wire::OrdStatus::Cancelled;
        case OrderState::PendingReplace: return wire::OrdStatus::PendingReplace;
        case OrderState::Replaced:       return wire::OrdStatus::Replaced;
        case OrderState::Rejected:       return wire::OrdStatus::Rejected;
        case OrderState::Expired:        return wire::OrdStatus::Expired;
        case OrderState::DoneForDay:     return wire::OrdStatus::DoneForDay;
        case OrderState::Count:          return wire::OrdStatus::Rejected;
    }
    return wire::OrdStatus::Rejected;
}

void OmsCore::emit_client_exec_report(
    uint64_t order_id,
    wire::ExecType exec_type,
    wire::OrdStatus ord_status,
    double leaves_qty,
    double cum_qty,
    double last_qty,
    double last_px,
    uint64_t mono_ts_ns) noexcept {

    wire::ExecReportEventWire er{};
    er.order_id = order_id;
    er.exec_type = exec_type;
    er.ord_status = ord_status;
    er.leaves_qty = leaves_qty;
    er.cum_qty = cum_qty;
    er.last_qty = last_qty;
    er.last_px = last_px;
    er.mono_ts_ns = mono_ts_ns;

    std::snprintf(er.exec_id, sizeof(er.exec_id), "E-%lu", ++exec_id_seq_);

    static_cast<void>(client_exec_reports_.try_push(er));
}

void OmsCore::process_inbound_order(const wire::OrderEventWire& wire) noexcept {
    const uint64_t now = current_mono_ns();

    // FR-014: Duplicate ClOrdID check
    if (order_pool_.find_by_cl_ord_id(wire.cl_ord_id).has_value()) {
        orders_rejected_.fetch_add(1, std::memory_order_relaxed);
        emit_client_exec_report(0, wire::ExecType::Rejected, wire::OrdStatus::Rejected,
                                0.0, 0.0, 0.0, 0.0, now);
        if (now >= wire.mono_ts_ns) latency_histogram_.record(now - wire.mono_ts_ns);
        return;
    }

    // Allocate from OrderPool
    auto order_id_opt = order_pool_.allocate();
    if (!order_id_opt.has_value()) {
        // Pool exhaustion: reject new order
        orders_rejected_.fetch_add(1, std::memory_order_relaxed);
        emit_client_exec_report(0, wire::ExecType::Rejected, wire::OrdStatus::Rejected,
                                0.0, 0.0, 0.0, 0.0, now);
        if (now >= wire.mono_ts_ns) latency_histogram_.record(now - wire.mono_ts_ns);
        return;
    }

    const uint64_t order_id = *order_id_opt;
    Order& order = order_pool_.get(order_id);
    order.order_id = order_id;
    std::memcpy(order.cl_ord_id, wire.cl_ord_id, sizeof(order.cl_ord_id));
    order.cl_ord_id[sizeof(order.cl_ord_id) - 1] = '\0';
    std::memcpy(order.symbol, wire.symbol, sizeof(order.symbol));
    order.symbol[sizeof(order.symbol) - 1] = '\0';
    order.side = wire.side;
    order.order_type = wire.order_type;
    order.time_in_force = wire.time_in_force;
    order.price = wire.price;
    order.quantity = wire.quantity;
    order.cum_qty = 0.0;
    order.leaves_qty = wire.quantity;
    order.avg_px = 0.0;
    order.session_id = wire.session_id;
    order.created_at_mono_ns = wire.mono_ts_ns;
    order.updated_at_mono_ns = now;
    order.state = OrderState::PendingNew;

    static_cast<void>(order_pool_.register_cl_ord_id(wire.cl_ord_id, order_id));

    // FR-016: Emit execution report for state transition (PendingNew)
    emit_client_exec_report(order_id, wire::ExecType::New, wire::OrdStatus::PendingNew,
                            order.leaves_qty, 0.0, 0.0, 0.0, now);

    // Push to outbound routing buffer
    wire::OrderEventWire routed = wire;
    static_cast<void>(outbound_orders_.try_push(routed));

    orders_processed_.fetch_add(1, std::memory_order_relaxed);
    if (now >= wire.mono_ts_ns) {
        latency_histogram_.record(now - wire.mono_ts_ns);
    }
}

void OmsCore::process_inbound_cancel(const wire::CancelEventWire& wire) noexcept {
    const uint64_t now = current_mono_ns();

    // Match via OrigClOrdID
    auto orig_id_opt = order_pool_.find_by_cl_ord_id(wire.orig_cl_ord_id);
    if (!orig_id_opt.has_value()) {
        // Unknown order: reject cancel
        orders_rejected_.fetch_add(1, std::memory_order_relaxed);
        emit_client_exec_report(0, wire::ExecType::Rejected, wire::OrdStatus::Rejected,
                                0.0, 0.0, 0.0, 0.0, now);
        if (now >= wire.mono_ts_ns) latency_histogram_.record(now - wire.mono_ts_ns);
        return;
    }

    Order& order = order_pool_.get(*orig_id_opt);
    if (!is_legal_transition(order.state, OrderEvent::CancelRequest)) {
        // State does not allow cancel (e.g., terminal state)
        orders_rejected_.fetch_add(1, std::memory_order_relaxed);
        emit_client_exec_report(order.order_id, wire::ExecType::Rejected,
                                to_wire_ord_status(order.state),
                                order.leaves_qty, order.cum_qty, 0.0, 0.0, now);
        if (now >= wire.mono_ts_ns) latency_histogram_.record(now - wire.mono_ts_ns);
        return;
    }

    // Transition to PendingCancel
    order.state = transition(order.state, OrderEvent::CancelRequest);
    std::memcpy(order.orig_cl_ord_id, wire.orig_cl_ord_id, sizeof(order.orig_cl_ord_id));
    order.orig_cl_ord_id[sizeof(order.orig_cl_ord_id) - 1] = '\0';
    order.updated_at_mono_ns = now;

    // FR-016: Emit execution report for state transition (PendingCancel)
    emit_client_exec_report(order.order_id, wire::ExecType::New, wire::OrdStatus::PendingCancel,
                            order.leaves_qty, order.cum_qty, 0.0, 0.0, now);

    // Forward cancel to venue
    static_cast<void>(outbound_cancels_.try_push(wire));

    cancels_processed_.fetch_add(1, std::memory_order_relaxed);
    if (now >= wire.mono_ts_ns) {
        latency_histogram_.record(now - wire.mono_ts_ns);
    }
}

void OmsCore::process_venue_exec_report(const wire::ExecReportEventWire& wire) noexcept {
    const uint64_t now = current_mono_ns();

    if (wire.order_id >= order_pool_.size()) {
        return;
    }

    Order& order = order_pool_.get(wire.order_id);

    // Map ExecType to OrderEvent
    OrderEvent ev = OrderEvent::RejectEvent;
    switch (wire.exec_type) {
        case wire::ExecType::New:
            ev = OrderEvent::Ack;
            break;
        case wire::ExecType::PartialFill:
            ev = OrderEvent::PartialFill;
            break;
        case wire::ExecType::Fill:
            ev = OrderEvent::FullFill;
            break;
        case wire::ExecType::Cancelled:
            ev = OrderEvent::CancelAck;
            break;
        case wire::ExecType::Rejected:
            ev = OrderEvent::RejectEvent;
            break;
        case wire::ExecType::Expired:
            ev = OrderEvent::ExpireEvent;
            break;
        default:
            ev = OrderEvent::RejectEvent;
            break;
    }

    if (is_legal_transition(order.state, ev)) {
        order.state = transition(order.state, ev);
    }

    // Update quantities and price
    if (wire.exec_type == wire::ExecType::PartialFill || wire.exec_type == wire::ExecType::Fill) {
        order.apply_fill(wire.last_qty, wire.last_px, wire.mono_ts_ns);
    } else if (wire.exec_type == wire::ExecType::Cancelled) {
        order.leaves_qty = 0.0;
        order.updated_at_mono_ns = wire.mono_ts_ns;
    } else {
        order.updated_at_mono_ns = wire.mono_ts_ns;
    }

    assert(order.check_quantity_invariant());

    // FR-016: Emit execution report for client
    emit_client_exec_report(order.order_id, wire.exec_type, to_wire_ord_status(order.state),
                            order.leaves_qty, order.cum_qty, wire.last_qty, wire.last_px, now);

    reports_processed_.fetch_add(1, std::memory_order_relaxed);
    if (now >= wire.mono_ts_ns) {
        latency_histogram_.record(now - wire.mono_ts_ns);
    }
}

size_t OmsCore::poll_once() noexcept {
    size_t count = 0;

    wire::OrderEventWire order{};
    if (inbound_orders_.try_pop(order)) {
        process_inbound_order(order);
        ++count;
    }

    wire::CancelEventWire cancel{};
    if (inbound_cancels_.try_pop(cancel)) {
        process_inbound_cancel(cancel);
        ++count;
    }

    wire::ExecReportEventWire rep{};
    if (venue_exec_reports_.try_pop(rep)) {
        process_venue_exec_report(rep);
        ++count;
    }

    return count;
}

size_t OmsCore::poll_all() noexcept {
    size_t total = 0;
    size_t n = 0;
    do {
        n = poll_once();
        total += n;
    } while (n > 0);
    return total;
}

void OmsCore::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    worker_thread_ = std::jthread([this](std::stop_token st) {
        while (!st.stop_requested() && running_.load(std::memory_order_relaxed)) {
            size_t processed = poll_once();
            if (processed == 0) {
#if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
#else
                std::this_thread::yield();
#endif
            }
        }
    });
}

void OmsCore::stop() {
    if (running_.exchange(false, std::memory_order_acq_rel)) {
        if (worker_thread_.joinable()) {
            worker_thread_.request_stop();
            worker_thread_.join();
        }
    }
}

} // namespace kestrel::oms
