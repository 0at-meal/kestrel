#include "kestrel/gateway/inbound_gateway.h"
#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <quickfix/FieldMap.h>

namespace kestrel::gateway {

InboundGatewayApplication::InboundGatewayApplication(
    InboundOrderBuffer& order_buf,
    InboundCancelBuffer& cancel_buf,
    OutboundExecBuffer& exec_buf) noexcept
    : order_buffer_(order_buf),
      cancel_buffer_(cancel_buf),
      exec_buffer_(exec_buf) {}

uint64_t InboundGatewayApplication::current_mono_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

void InboundGatewayApplication::onCreate(const FIX::SessionID&) {}

void InboundGatewayApplication::onLogon(const FIX::SessionID&) {
    is_logged_on_.store(true, std::memory_order_release);
}

void InboundGatewayApplication::onLogout(const FIX::SessionID&) {
    is_logged_on_.store(false, std::memory_order_release);
}

void InboundGatewayApplication::toAdmin(FIX::Message&, const FIX::SessionID&) {}

void InboundGatewayApplication::toApp(FIX::Message&, const FIX::SessionID&) {}

void InboundGatewayApplication::fromAdmin(const FIX::Message&, const FIX::SessionID&) {}

void InboundGatewayApplication::fromApp(const FIX::Message& msg, const FIX::SessionID& sessionID) {
    const uint64_t mono_ts_ns = current_mono_ns();
    const uint32_t session_num = static_cast<uint32_t>(std::hash<std::string>{}(sessionID.toString()));

    try {
        const FIX::MsgType msgType = msg.getHeader().getField(FIX::FIELD::MsgType);
        const std::string& type_str = msgType.getValue();

        if (type_str == FIX::MsgType_NewOrderSingle) {
            if (parse_new_order_single(msg, session_num, mono_ts_ns)) {
                orders_received_.fetch_add(1, std::memory_order_relaxed);
            } else {
                validation_errors_.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (type_str == FIX::MsgType_OrderCancelRequest) {
            if (parse_order_cancel_request(msg, session_num, mono_ts_ns)) {
                cancels_received_.fetch_add(1, std::memory_order_relaxed);
            } else {
                validation_errors_.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            // Unsupported app message
            validation_errors_.fetch_add(1, std::memory_order_relaxed);
        }
    } catch (const std::exception& e) {
        // FR-003: Reject and log any syntax/validation error without crashing
        validation_errors_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool InboundGatewayApplication::parse_new_order_single(
    const FIX::Message& msg, uint32_t session_num, uint64_t mono_ts_ns) {

    if (!msg.isSetField(FIX::FIELD::ClOrdID) ||
        !msg.isSetField(FIX::FIELD::Symbol) ||
        !msg.isSetField(FIX::FIELD::Side) ||
        !msg.isSetField(FIX::FIELD::OrdType) ||
        !msg.isSetField(FIX::FIELD::OrderQty)) {
        return false;
    }

    wire::OrderEventWire wire{};
    wire.mono_ts_ns = mono_ts_ns;
    wire.session_id = session_num;

    const std::string& cl_ord_id = msg.getField(FIX::FIELD::ClOrdID);
    if (cl_ord_id.empty()) return false;
    std::strncpy(wire.cl_ord_id, cl_ord_id.c_str(), sizeof(wire.cl_ord_id) - 1);

    const std::string& symbol = msg.getField(FIX::FIELD::Symbol);
    if (symbol.empty()) return false;
    std::strncpy(wire.symbol, symbol.c_str(), sizeof(wire.symbol) - 1);

    const char side_char = msg.getField(FIX::FIELD::Side)[0];
    if (side_char == FIX::Side_BUY) {
        wire.side = wire::Side::Buy;
    } else if (side_char == FIX::Side_SELL) {
        wire.side = wire::Side::Sell;
    } else {
        return false;
    }

    const char type_char = msg.getField(FIX::FIELD::OrdType)[0];
    if (type_char == FIX::OrdType_MARKET) {
        wire.order_type = wire::OrderType::Market;
    } else if (type_char == FIX::OrdType_LIMIT) {
        wire.order_type = wire::OrderType::Limit;
        if (!msg.isSetField(FIX::FIELD::Price)) {
            return false;
        }
        wire.price = std::stod(msg.getField(FIX::FIELD::Price));
        if (wire.price <= 0.0) return false;
    } else {
        return false;
    }

    wire.quantity = std::stod(msg.getField(FIX::FIELD::OrderQty));
    if (wire.quantity <= 0.0) {
        return false;
    }

    if (msg.isSetField(FIX::FIELD::TimeInForce)) {
        const char tif_char = msg.getField(FIX::FIELD::TimeInForce)[0];
        if (tif_char == FIX::TimeInForce_DAY) {
            wire.time_in_force = wire::TimeInForce::Day;
        } else if (tif_char == FIX::TimeInForce_GOOD_TILL_CANCEL) {
            wire.time_in_force = wire::TimeInForce::GTC;
        }
    }

    return order_buffer_.try_push(wire);
}

bool InboundGatewayApplication::parse_order_cancel_request(
    const FIX::Message& msg, uint32_t session_num, uint64_t mono_ts_ns) {

    if (!msg.isSetField(FIX::FIELD::ClOrdID) || !msg.isSetField(FIX::FIELD::OrigClOrdID)) {
        return false;
    }

    wire::CancelEventWire wire{};
    wire.mono_ts_ns = mono_ts_ns;
    wire.session_id = session_num;

    const std::string& cl_ord_id = msg.getField(FIX::FIELD::ClOrdID);
    if (cl_ord_id.empty()) return false;
    std::strncpy(wire.cl_ord_id, cl_ord_id.c_str(), sizeof(wire.cl_ord_id) - 1);

    const std::string& orig_cl_ord_id = msg.getField(FIX::FIELD::OrigClOrdID);
    if (orig_cl_ord_id.empty()) return false;
    std::strncpy(wire.orig_cl_ord_id, orig_cl_ord_id.c_str(), sizeof(wire.orig_cl_ord_id) - 1);

    return cancel_buffer_.try_push(wire);
}

size_t InboundGatewayApplication::drain_outbound(const FIX::SessionID& sessionID) {
    size_t count = 0;
    wire::ExecReportEventWire er{};

    while (exec_buffer_.try_pop(er)) {
        FIX44::ExecutionReport report(
            FIX::OrderID(std::to_string(er.order_id)),
            FIX::ExecID(er.exec_id),
            FIX::ExecType(FIX::ExecType_NEW),
            FIX::OrdStatus(FIX::OrdStatus_NEW),
            FIX::Side(FIX::Side_BUY),
            FIX::LeavesQty(er.leaves_qty),
            FIX::CumQty(er.cum_qty),
            FIX::AvgPx(er.last_px)
        );

        // Map ExecType
        switch (er.exec_type) {
            case wire::ExecType::New:
                report.setField(FIX::ExecType(FIX::ExecType_NEW));
                break;
            case wire::ExecType::PartialFill:
                report.setField(FIX::ExecType(FIX::ExecType_PARTIAL_FILL));
                break;
            case wire::ExecType::Fill:
                report.setField(FIX::ExecType(FIX::ExecType_FILL));
                break;
            case wire::ExecType::Cancelled:
                report.setField(FIX::ExecType(FIX::ExecType_CANCELED));
                break;
            case wire::ExecType::Replaced:
                report.setField(FIX::ExecType(FIX::ExecType_REPLACED));
                break;
            case wire::ExecType::Rejected:
                report.setField(FIX::ExecType(FIX::ExecType_REJECTED));
                break;
            case wire::ExecType::Expired:
                report.setField(FIX::ExecType(FIX::ExecType_EXPIRED));
                break;
            case wire::ExecType::DoneForDay:
                report.setField(FIX::ExecType(FIX::ExecType_DONE_FOR_DAY));
                break;
            case wire::ExecType::Count:
                break;
        }

        // Map OrdStatus
        switch (er.ord_status) {
            case wire::OrdStatus::PendingNew:
                report.setField(FIX::OrdStatus(FIX::OrdStatus_PENDING_NEW));
                break;
            case wire::OrdStatus::New:
                report.setField(FIX::OrdStatus(FIX::OrdStatus_NEW));
                break;
            case wire::OrdStatus::PartiallyFilled:
                report.setField(FIX::OrdStatus(FIX::OrdStatus_PARTIALLY_FILLED));
                break;
            case wire::OrdStatus::Filled:
                report.setField(FIX::OrdStatus(FIX::OrdStatus_FILLED));
                break;
            case wire::OrdStatus::PendingCancel:
                report.setField(FIX::OrdStatus(FIX::OrdStatus_PENDING_CANCEL));
                break;
            case wire::OrdStatus::Cancelled:
                report.setField(FIX::OrdStatus(FIX::OrdStatus_CANCELED));
                break;
            case wire::OrdStatus::PendingReplace:
                report.setField(FIX::OrdStatus(FIX::OrdStatus_PENDING_REPLACE));
                break;
            case wire::OrdStatus::Replaced:
                report.setField(FIX::OrdStatus(FIX::OrdStatus_REPLACED));
                break;
            case wire::OrdStatus::Rejected:
                report.setField(FIX::OrdStatus(FIX::OrdStatus_REJECTED));
                break;
            case wire::OrdStatus::Expired:
                report.setField(FIX::OrdStatus(FIX::OrdStatus_EXPIRED));
                break;
            case wire::OrdStatus::DoneForDay:
                report.setField(FIX::OrdStatus(FIX::OrdStatus_DONE_FOR_DAY));
                break;
            case wire::OrdStatus::Count:
                break;
        }

        report.setField(FIX::LastQty(er.last_qty));
        report.setField(FIX::LastPx(er.last_px));

        try {
            FIX::Session::sendToTarget(report, sessionID);
            reports_sent_.fetch_add(1, std::memory_order_relaxed);
            ++count;
        } catch (const FIX::SessionNotFound&) {
            // Session not connected in unit test or disconnected
        }
    }

    return count;
}

} // namespace kestrel::gateway
