#include "kestrel/venue/stub_venue.h"
#include <iostream>

namespace kestrel::venue {

void StubVenueApplication::onCreate(const FIX::SessionID&) {}

void StubVenueApplication::onLogon(const FIX::SessionID&) {
    is_logged_on_.store(true, std::memory_order_release);
}

void StubVenueApplication::onLogout(const FIX::SessionID&) {
    is_logged_on_.store(false, std::memory_order_release);
}

void StubVenueApplication::toAdmin(FIX::Message&, const FIX::SessionID&) {}

void StubVenueApplication::toApp(FIX::Message&, const FIX::SessionID&) {}

void StubVenueApplication::fromAdmin(const FIX::Message&, const FIX::SessionID&) {}

void StubVenueApplication::fromApp(const FIX::Message& msg, const FIX::SessionID& sessionID) {
    try {
        const FIX::MsgType msgType = msg.getHeader().getField(FIX::FIELD::MsgType);
        const std::string& type_str = msgType.getValue();

        if (type_str == FIX::MsgType_NewOrderSingle) {
            handle_new_order_single(msg, sessionID);
        } else if (type_str == FIX::MsgType_OrderCancelRequest) {
            handle_order_cancel_request(msg, sessionID);
        }
    } catch (const std::exception&) {
        // Safe rejection / ignore malformed message
    }
}

void StubVenueApplication::send_or_record_report(
    FIX44::ExecutionReport report, const FIX::SessionID& sessionID) {
    generated_reports_.push_back(report);
    try {
        FIX::Session::sendToTarget(report, sessionID);
        reports_sent_.fetch_add(1, std::memory_order_relaxed);
    } catch (const FIX::SessionNotFound&) {
        // Session offline in unit tests
    }
}

void StubVenueApplication::handle_new_order_single(
    const FIX::Message& msg, const FIX::SessionID& sessionID) {
    if (!msg.isSetField(FIX::FIELD::ClOrdID) ||
        !msg.isSetField(FIX::FIELD::Side) ||
        !msg.isSetField(FIX::FIELD::OrderQty)) {
        return;
    }

    orders_received_.fetch_add(1, std::memory_order_relaxed);

    const std::string& cl_ord_id = msg.getField(FIX::FIELD::ClOrdID);
    const char side_char = msg.getField(FIX::FIELD::Side)[0];
    const double qty = std::stod(msg.getField(FIX::FIELD::OrderQty));
    const std::string symbol = msg.isSetField(FIX::FIELD::Symbol) ? msg.getField(FIX::FIELD::Symbol) : "BTC/USDT";

    double price = 0.0;
    if (msg.isSetField(FIX::FIELD::Price)) {
        price = std::stod(msg.getField(FIX::FIELD::Price));
    }
    const double fill_px = (price > 0.0) ? price : 100.0;

    const std::string venue_order_id = "V-ORD-" + std::to_string(++order_seq_);

    // 1. Immediately send ExecType::New (Ack)
    {
        const std::string exec_id_ack = "V-EXEC-" + std::to_string(++exec_seq_);
        FIX44::ExecutionReport ack(
            FIX::OrderID(venue_order_id),
            FIX::ExecID(exec_id_ack),
            FIX::ExecType(FIX::ExecType_NEW),
            FIX::OrdStatus(FIX::OrdStatus_NEW),
            FIX::Side(side_char),
            FIX::LeavesQty(qty),
            FIX::CumQty(0.0),
            FIX::AvgPx(0.0)
        );
        ack.setField(FIX::ClOrdID(cl_ord_id));
        ack.setField(FIX::Symbol(symbol));
        ack.setField(FIX::OrderQty(qty));
        ack.setField(FIX::LastQty(0.0));
        ack.setField(FIX::LastPx(0.0));

        send_or_record_report(ack, sessionID);
    }

    // 2. Immediately send ExecType::Fill (Fill)
    {
        const std::string exec_id_fill = "V-EXEC-" + std::to_string(++exec_seq_);
        FIX44::ExecutionReport fill(
            FIX::OrderID(venue_order_id),
            FIX::ExecID(exec_id_fill),
            FIX::ExecType(FIX::ExecType_FILL),
            FIX::OrdStatus(FIX::OrdStatus_FILLED),
            FIX::Side(side_char),
            FIX::LeavesQty(0.0),
            FIX::CumQty(qty),
            FIX::AvgPx(fill_px)
        );
        fill.setField(FIX::ClOrdID(cl_ord_id));
        fill.setField(FIX::Symbol(symbol));
        fill.setField(FIX::OrderQty(qty));
        fill.setField(FIX::LastQty(qty));
        fill.setField(FIX::LastPx(fill_px));

        send_or_record_report(fill, sessionID);
    }
}

void StubVenueApplication::handle_order_cancel_request(
    const FIX::Message& msg, const FIX::SessionID& sessionID) {
    if (!msg.isSetField(FIX::FIELD::ClOrdID) || !msg.isSetField(FIX::FIELD::OrigClOrdID)) {
        return;
    }

    cancels_received_.fetch_add(1, std::memory_order_relaxed);

    const std::string& cl_ord_id = msg.getField(FIX::FIELD::ClOrdID);
    const std::string& orig_cl_ord_id = msg.getField(FIX::FIELD::OrigClOrdID);
    const char side_char = msg.isSetField(FIX::FIELD::Side) ? msg.getField(FIX::FIELD::Side)[0] : FIX::Side_BUY;
    const std::string venue_order_id = "V-ORD-CANC-" + std::to_string(++order_seq_);
    const std::string exec_id_cancel = "V-EXEC-" + std::to_string(++exec_seq_);

    FIX44::ExecutionReport cancel_rep(
        FIX::OrderID(venue_order_id),
        FIX::ExecID(exec_id_cancel),
        FIX::ExecType(FIX::ExecType_CANCELED),
        FIX::OrdStatus(FIX::OrdStatus_CANCELED),
        FIX::Side(side_char),
        FIX::LeavesQty(0.0),
        FIX::CumQty(0.0),
        FIX::AvgPx(0.0)
    );
    cancel_rep.setField(FIX::ClOrdID(cl_ord_id));
    cancel_rep.setField(FIX::OrigClOrdID(orig_cl_ord_id));

    send_or_record_report(cancel_rep, sessionID);
}

} // namespace kestrel::venue
