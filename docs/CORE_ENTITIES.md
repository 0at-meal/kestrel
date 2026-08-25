# Core Entities & Relationships — OMS Core + FIX Gateways

**Document ID:** DESIGN-001
**Status:** Draft v1
**Traces to:** 01_REQUIREMENTS.md (FR-010 to FR-052)

---

## 1. A note on two representations, not one

Before the entity list: every entity below has **two possible forms** in the system,
and conflating them is the single most common mistake in a project like this.

- **Domain form** — a rich, safe, possibly-heap-backed object used in non-hot-path
  code: config loading, position ledger, replay/audit tooling, unit tests. This is
  what's described below.
- **Wire/pipeline form** — a flat, fixed-size POD struct with no pointers, no
  virtual dispatch, no dynamic allocation, passed by value through the lock-free
  ring buffers between pipeline stages (per NFR-003, NFR-010). E.g. `Order` the
  domain entity might own a `std::string` symbol and a vector of exec report
  pointers; `OrderEvent` the wire struct has a fixed `char symbol[12]` and no
  reports attached at all.

Every entity that appears on the hot path (`Order`, `ExecutionReport`,
`MarketDataTick`, risk check results) needs both forms defined before implementation
starts. This isn't premature optimization — it's the actual architectural decision
that determines whether the concurrency model in the Charter is achievable at all.
Entities that never touch the hot path (`Venue` config, `RiskLimit` config,
`CaptureRecord` once written) only need the domain form.

---

## 2. Entity catalogue

### 2.1 Order
The central entity. One `Order` exists per accepted New Order Single, and persists
(in memory, and optionally on disk per FR-005/NFR-030) through its entire lifecycle.

| Field | Type | Notes |
|---|---|---|
| `order_id` | internal UUID/sequence | System-generated, PK, never reused |
| `cl_ord_id` | string | Client-assigned, unique per session (FR-014) |
| `orig_cl_ord_id` | string, nullable | Set on Cancel/Replace chains (FR-013) |
| `session_id` | FK -> FixSession | Which session owns this order |
| `symbol` | FK -> Instrument | What's being traded |
| `venue_id` | FK -> Venue | Where it was routed |
| `side` | enum | Buy/Sell |
| `order_type` | enum | Market/Limit/etc |
| `price`, `quantity` | decimal | |
| `state` | enum | See state machine, FR-011 |
| `cum_qty`, `leaves_qty`, `avg_px` | decimal | Derived, updated on each fill |
| `created_at`, `updated_at` | timestamp | |

**Invariant:** `cum_qty + leaves_qty == quantity` must hold after every execution
report is applied. This is a good candidate for an assertion in the OMS core, not
just a design comment — invariant violations here mean state-machine or arithmetic
bugs, exactly the class of bug FR-012's exhaustive transition table exists to prevent.

### 2.2 ExecutionReport
Immutable, append-only. One per state transition on an order (not just fills — an
ack is also an ExecutionReport, per FR-016). This *is* the order's audit trail; the
order's current state is technically a fold over its ExecutionReports.

| Field | Type | Notes |
|---|---|---|
| `exec_id` | PK | |
| `order_id` | FK -> Order | |
| `exec_type` | enum | New/PartialFill/Fill/Cancelled/Replaced/Rejected/... |
| `ord_status` | enum | Resulting order state after this report |
| `last_qty`, `last_px` | decimal, nullable | Only set on fill-type reports |
| `ts` | timestamp | |

### 2.3 Instrument
Reference data. Static-ish, loaded at startup, rarely mutated at runtime.

| Field | Type | Notes |
|---|---|---|
| `symbol` | PK | |
| `security_type` | enum | |
| `tick_size`, `lot_size` | decimal | Used by risk checks (FR-020/022) and fill logic |
| `currency` | string | |

### 2.4 Position
One row per instrument, continuously updated as fills arrive (FR-015). This is a
**derived/aggregate entity** — logically a fold over `ExecutionReport`s of type
Fill for that symbol — even if it's also materialized as a live table for O(1) lookup.

| Field | Type | Notes |
|---|---|---|
| `symbol` | FK -> Instrument, effectively PK (one position per symbol) | |
| `net_qty` | decimal | |
| `avg_price` | decimal | |
| `realized_pnl` | decimal | |
| `updated_at` | timestamp | |

### 2.5 FixSession
One per FIX connection — you'll have at least two live at once (inbound gateway's
session with the client, outbound gateway's session with the venue).

| Field | Type | Notes |
|---|---|---|
| `session_id` | PK | |
| `sender_comp_id`, `target_comp_id` | string | FIX session identity |
| `direction` | enum | Inbound / Outbound |
| `seq_out`, `seq_in` | int | Persisted per FR-005 |
| `state` | enum | Disconnected/LoggingOn/LoggedOn |

### 2.6 Venue
Represents a counterparty an order can be routed to. In v1 there's exactly one
instance — the simulated venue — but it's modeled as an entity, not a hardcoded
singleton, specifically to satisfy FR-033 (swappable venue).

| Field | Type | Notes |
|---|---|---|
| `venue_id` | PK | |
| `name` | string | |
| `fill_model` | config/enum | Immediate-full / probabilistic-partial / latency-delayed (open question from REQ-001 §5) |

### 2.7 MarketDataTick
Normalized market data, regardless of source. Feeds the venue simulator's reference
price (FR-031) and, optionally, an internal FIX MarketData translation (FR-042).

| Field | Type | Notes |
|---|---|---|
| `symbol` | FK -> Instrument | |
| `bid_px`, `ask_px`, `last_px` | decimal | |
| `ts` | timestamp | Source timestamp, not receipt timestamp — keep both if they diverge |
| `source` | string | e.g. "binance-ws" — traceability back to FR-040 |

### 2.8 RiskLimit
Configuration entity, not really a "domain object" in the transactional sense —
but modeled explicitly (rather than as scattered constants) so FR-024's requirement
("every rejection attributable to a specific, logged rule") is structurally
enforceable: a rejection always references a `limit_id`.

| Field | Type | Notes |
|---|---|---|
| `limit_id` | PK | |
| `session_id` | FK -> FixSession, nullable (null = global default) | |
| `max_notional`, `max_qty`, `fat_finger_pct` | decimal | |

### 2.9 CaptureRecord
The raw FIX capture entity backing FR-050/051 (capture & deterministic replay).
Every inbound and outbound FIX message gets one row — this is intentionally more
granular than ExecutionReport, since it captures session-level noise (heartbeats,
resend requests) that never becomes a domain-level order event.

| Field | Type | Notes |
|---|---|---|
| `record_id` | PK | |
| `session_id` | FK -> FixSession | |
| `order_id` | FK -> Order, nullable | Null for session-level messages (heartbeat etc.) |
| `direction` | enum | Inbound / Outbound |
| `mono_ts` | monotonic timestamp | For deterministic replay ordering — not wall clock |
| `raw_fix` | bytes/string | The actual wire message |

---

## 3. Relationship summary

| From | To | Cardinality | Meaning |
|---|---|---|---|
| FixSession | Order | 1 -> N | A session submits many orders |
| Order | ExecutionReport | 1 -> N | Every state transition appends one report |
| Instrument | Order | 1 -> N | Every order references one instrument |
| Instrument | Position | 1 -> 1 | One live position per instrument |
| Venue | Order | 1 -> N | Every order is routed to exactly one venue |
| Venue | MarketDataTick | 1 -> N | Venue's reference price is fed by ticks |
| Instrument | MarketDataTick | 1 -> N | Ticks are per-instrument |
| FixSession | RiskLimit | 1 -> N | A session can have session-specific limits (or fall back to global) |
| FixSession | CaptureRecord | 1 -> N | Every message on a session is captured |
| Order | CaptureRecord | 1 -> N | A raw capture record may correlate to the order it caused (nullable) |
| Order | Order (self) | 0..1 -> 0..1 | Cancel/Replace chain via `orig_cl_ord_id` |

Rendered as an ER diagram below.

---

## 4. Open design note carried forward

`RiskLimit` is modeled as data, not code, on purpose — this keeps FR-024's audit
requirement cheap to satisfy and means adding a new risk rule later is a config
change plus one new evaluator function, not a scattered edit across the order
acceptance path. Worth revisiting once real risk rules are implemented, in case a
given rule doesn't fit the flat-limit shape (e.g. a rule that depends on current
position, not just the incoming order).

**Next suggested task:** pipeline / threading architecture — mapping these entities
onto the LMAX-style stage pipeline from the Charter (which stage owns which entity,
what crosses a ring buffer as a wire-form struct vs what stays domain-form only).
