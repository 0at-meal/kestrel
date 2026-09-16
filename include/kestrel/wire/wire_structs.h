#pragma once

#include <cstdint>
#include <type_traits>
#include "kestrel/wire/enums.h"

namespace kestrel::wire {

struct OrderEventWire {
    char        cl_ord_id[20]{};
    char        symbol[12]{};
    Side        side{Side::Buy};
    OrderType   order_type{OrderType::Limit};
    TimeInForce time_in_force{TimeInForce::Day};
    uint8_t     _pad0[5]{};
    double      price{0.0};
    double      quantity{0.0};
    uint32_t    session_id{0};
    uint32_t    _pad1{0};
    uint64_t    mono_ts_ns{0};
};

static_assert(std::is_standard_layout_v<OrderEventWire>, "OrderEventWire must be standard layout");
static_assert(std::is_trivially_copyable_v<OrderEventWire>, "OrderEventWire must be trivially copyable");

struct CancelEventWire {
    char        cl_ord_id[20]{};
    char        orig_cl_ord_id[20]{};
    uint32_t    session_id{0};
    uint32_t    _pad0{0};
    uint64_t    mono_ts_ns{0};
};

static_assert(std::is_standard_layout_v<CancelEventWire>, "CancelEventWire must be standard layout");
static_assert(std::is_trivially_copyable_v<CancelEventWire>, "CancelEventWire must be trivially copyable");

struct ExecReportEventWire {
    char        exec_id[20]{};
    uint64_t    order_id{0};      // Internal pool index / sequence
    ExecType    exec_type{ExecType::New};
    OrdStatus   ord_status{OrdStatus::New};
    uint8_t     _pad0[6]{};
    double      last_qty{0.0};
    double      last_px{0.0};
    double      cum_qty{0.0};
    double      leaves_qty{0.0};
    uint64_t    mono_ts_ns{0};
};

static_assert(std::is_standard_layout_v<ExecReportEventWire>, "ExecReportEventWire must be standard layout");
static_assert(std::is_trivially_copyable_v<ExecReportEventWire>, "ExecReportEventWire must be trivially copyable");

struct MarketDataEventWire {
    char        symbol[12]{};
    uint8_t     _pad0[4]{};
    double      bid_px{0.0};
    double      ask_px{0.0};
    double      last_px{0.0};
    uint64_t    mono_ts_ns{0};
};

static_assert(std::is_standard_layout_v<MarketDataEventWire>, "MarketDataEventWire must be standard layout");
static_assert(std::is_trivially_copyable_v<MarketDataEventWire>, "MarketDataEventWire must be trivially copyable");

} // namespace kestrel::wire
