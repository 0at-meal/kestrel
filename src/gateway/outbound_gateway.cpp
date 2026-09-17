#include "kestrel/gateway/outbound_gateway.h"
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <quickfix/FieldMap.h>

namespace kestrel::gateway {

OutboundGatewayApplication::OutboundGatewayApplication(
    OutboundOrderBuffer& order_buf,
    OutboundCancelBuffer& cancel_buf,
    InboundExecBuffer& exec_buf) noexcept
    : order_buffer_(order_buf),
      cancel_buffer_(cancel_buf),
      exec_buffer_(exec_buf) {}

uint64_t OutboundGatewayApplication::current_mono_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t OutboundGatewayApplication::extract_order_id(const FIX::Message& msg) noexcept {
    auto parse_digits = [](const std::string& str) noexcept -> uint64_t {
        if (str.empty()) return 0;
        size_t start = 0;
        while (start < str.size() && !std::isdigit(static_cast<unsigned char>(str[start]))) {
            ++start;
        }
        if (start == str.size()) return 0;
        try {
            return std::stoull(str.substr(start));
        } catch (...) {
            return 0;
        }
    };

    if (msg.isSetField(FIX::FIELD::ClOrdID)) {
        uint64_t id = parse_digits(msg.getField(FIX::FIELD::ClOrdID));
        if (id != 0) return id;
    }
    if (msg.isSetField(FIX::FIELD::OrderID)) {
        return parse_digits(msg.getField(FIX::FIELD::OrderID));
    }
    return 0;
}

void OutboundGatewayApplication::onCreate(const FIX::SessionID&) {}

void OutboundGatewayApplication::onLogon(const FIX::SessionID&) {
    is_logged_on_.store(true, std::memory_order_release);
}

void OutboundGatewayApplication::onLogout(const FIX::SessionID&) {
    is_logged_on_.store(false, std::memory_order_release);
}

void OutboundGatewayApplication::toAdmin(FIX::Message&, const FIX::SessionID&) {}

void OutboundGatewayApplication::toApp(FIX::Message&, const FIX::SessionID&) {}

void OutboundGatewayApplication::fromAdmin(const FIX::Message&, const FIX::SessionID&) {}

void OutboundGatewayApplication::fromApp(const FIX::Message& msg, const FIX::SessionID&) {
    const uint64_t mono_ts_ns = current_mono_ns();

    try {
        const FIX::MsgType msgType = msg.getHeader().getField(FIX::FIELD::MsgType);
        const std::string& type_str = msgType.getValue();

        if (type_str == FIX::MsgType_ExecutionReport) {
            if (parse_execution_report(msg, mono_ts_ns)) {
                reports_received_.fetch_add(1, std::memory_order_relaxed);
            } else {
                validation_errors_.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            validation_errors_.fetch_add(1, std::memory_order_relaxed);
        }
    } catch (const std::exception&) {
        validation_errors_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool OutboundGatewayApplication::parse_execution_report(const FIX::Message& msg, uint64_t mono_ts_ns) {
    if (!msg.isSetField(FIX::FIELD::ExecID) ||
        !msg.isSetField(FIX::FIELD::ExecType) ||
        !msg.isSetField(FIX::FIELD::OrdStatus) ||
        !msg.isSetField(FIX::FIELD::CumQty) ||
        !msg.isSetField(FIX::FIELD::LeavesQty)) {
        return false;
    }

    if (!msg.isSetField(FIX::FIELD::ClOrdID) && !msg.isSetField(FIX::FIELD::OrderID)) {
        return false;
    }

    wire::ExecReportEventWire er{};
    er.mono_ts_ns = mono_ts_ns;
    er.order_id = extract_order_id(msg);

    const std::string& exec_id = msg.getField(FIX::FIELD::ExecID);
    if (exec_id.empty()) return false;
    std::strncpy(er.exec_id, exec_id.c_str(), sizeof(er.exec_id) - 1);

    const char exec_char = msg.getField(FIX::FIELD::ExecType)[0];
    switch (exec_char) {
        case FIX::ExecType_NEW:
            er.exec_type = wire::ExecType::New;
            break;
        case FIX::ExecType_PARTIAL_FILL:
            er.exec_type = wire::ExecType::PartialFill;
            break;
        case FIX::ExecType_FILL:
            er.exec_type = wire::ExecType::Fill;
            break;
        case FIX::ExecType_CANCELED:
            er.exec_type = wire::ExecType::Cancelled;
            break;
        case FIX::ExecType_REPLACED:
            er.exec_type = wire::ExecType::Replaced;
            break;
        case FIX::ExecType_REJECTED:
            er.exec_type = wire::ExecType::Rejected;
            break;
        case FIX::ExecType_EXPIRED:
            er.exec_type = wire::ExecType::Expired;
            break;
        case FIX::ExecType_DONE_FOR_DAY:
            er.exec_type = wire::ExecType::DoneForDay;
            break;
        default:
            return false;
    }

    const char status_char = msg.getField(FIX::FIELD::OrdStatus)[0];
    switch (status_char) {
        case FIX::OrdStatus_NEW:
            er.ord_status = wire::OrdStatus::New;
            break;
        case FIX::OrdStatus_PARTIALLY_FILLED:
            er.ord_status = wire::OrdStatus::PartiallyFilled;
            break;
        case FIX::OrdStatus_FILLED:
            er.ord_status = wire::OrdStatus::Filled;
            break;
        case FIX::OrdStatus_CANCELED:
            er.ord_status = wire::OrdStatus::Cancelled;
            break;
        case FIX::OrdStatus_REPLACED:
            er.ord_status = wire::OrdStatus::Replaced;
            break;
        case FIX::OrdStatus_REJECTED:
            er.ord_status = wire::OrdStatus::Rejected;
            break;
        case FIX::OrdStatus_PENDING_NEW:
            er.ord_status = wire::OrdStatus::PendingNew;
            break;
        case FIX::OrdStatus_PENDING_CANCEL:
            er.ord_status = wire::OrdStatus::PendingCancel;
            break;
        case FIX::OrdStatus_PENDING_REPLACE:
            er.ord_status = wire::OrdStatus::PendingReplace;
            break;
        case FIX::OrdStatus_EXPIRED:
            er.ord_status = wire::OrdStatus::Expired;
            break;
        case FIX::OrdStatus_DONE_FOR_DAY:
            er.ord_status = wire::OrdStatus::DoneForDay;
            break;
        default:
            return false;
    }

    er.cum_qty = std::stod(msg.getField(FIX::FIELD::CumQty));
    er.leaves_qty = std::stod(msg.getField(FIX::FIELD::LeavesQty));
    if (er.cum_qty < 0.0 || er.leaves_qty < 0.0) return false;

    if (msg.isSetField(FIX::FIELD::LastQty)) {
        er.last_qty = std::stod(msg.getField(FIX::FIELD::LastQty));
        if (er.last_qty < 0.0) return false;
    }
    if (msg.isSetField(FIX::FIELD::LastPx)) {
        er.last_px = std::stod(msg.getField(FIX::FIELD::LastPx));
        if (er.last_px < 0.0) return false;
    }

    return exec_buffer_.try_push(er);
}

size_t OutboundGatewayApplication::drain_outbound(const FIX::SessionID& venue_session) {
    size_t count = 0;

    // 1. Drain new orders from OMS Core
    wire::OrderEventWire order{};
    while (order_buffer_.try_pop(order)) {
        const char side_char = (order.side == wire::Side::Buy) ? FIX::Side_BUY : FIX::Side_SELL;
        const char type_char = (order.order_type == wire::OrderType::Market) ? FIX::OrdType_MARKET : FIX::OrdType_LIMIT;

        FIX44::NewOrderSingle nos(
            FIX::ClOrdID(order.cl_ord_id),
            FIX::Side(side_char),
            FIX::TransactTime(),
            FIX::OrdType(type_char)
        );

        nos.setField(FIX::Symbol(order.symbol));
        nos.setField(FIX::OrderQty(order.quantity));

        if (order.order_type == wire::OrderType::Limit) {
            nos.setField(FIX::Price(order.price));
        }

        if (order.time_in_force == wire::TimeInForce::GTC) {
            nos.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));
        } else {
            nos.setField(FIX::TimeInForce(FIX::TimeInForce_DAY));
        }

        try {
            FIX::Session::sendToTarget(nos, venue_session);
            orders_sent_.fetch_add(1, std::memory_order_relaxed);
            ++count;
        } catch (const FIX::SessionNotFound&) {
            // Venue session not connected
        }
    }

    // 2. Drain cancel requests from OMS Core
    wire::CancelEventWire cancel{};
    while (cancel_buffer_.try_pop(cancel)) {
        FIX44::OrderCancelRequest req(
            FIX::OrigClOrdID(cancel.orig_cl_ord_id),
            FIX::ClOrdID(cancel.cl_ord_id),
            FIX::Side(FIX::Side_BUY),
            FIX::TransactTime()
        );

        try {
            FIX::Session::sendToTarget(req, venue_session);
            cancels_sent_.fetch_add(1, std::memory_order_relaxed);
            ++count;
        } catch (const FIX::SessionNotFound&) {
            // Venue session not connected
        }
    }

    return count;
}

} // namespace kestrel::gateway
