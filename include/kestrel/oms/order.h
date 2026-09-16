#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>
#include "kestrel/wire/enums.h"
#include "kestrel/oms/order_state.h"

namespace kestrel::oms {

struct Order {
    uint64_t        order_id{0};
    char            cl_ord_id[20]{};
    char            orig_cl_ord_id[20]{};
    char            symbol[12]{};
    wire::Side      side{wire::Side::Buy};
    wire::OrderType order_type{wire::OrderType::Limit};
    wire::TimeInForce time_in_force{wire::TimeInForce::Day};
    double          price{0.0};
    double          quantity{0.0};
    OrderState      state{OrderState::PendingNew};
    double          cum_qty{0.0};
    double          leaves_qty{0.0};
    double          avg_px{0.0};
    uint32_t        session_id{0};
    uint64_t        created_at_mono_ns{0};
    uint64_t        updated_at_mono_ns{0};

    [[nodiscard]] bool check_quantity_invariant() const noexcept {
        constexpr double kEpsilon = 1e-7;
        return std::abs((cum_qty + leaves_qty) - quantity) < kEpsilon;
    }

    void apply_fill(double fill_qty, double fill_px, uint64_t mono_ts_ns) noexcept {
        if (fill_qty <= 0.0) {
            return;
        }
        const double new_cum = cum_qty + fill_qty;
        if (new_cum > 0.0) {
            avg_px = (avg_px * cum_qty + fill_px * fill_qty) / new_cum;
        }
        cum_qty = new_cum;
        leaves_qty = (quantity >= cum_qty) ? (quantity - cum_qty) : 0.0;
        updated_at_mono_ns = mono_ts_ns;
    }
};

} // namespace kestrel::oms
