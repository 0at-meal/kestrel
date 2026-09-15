# Kestrel — Implementation Tickets

**Derived from:** [PRD.md](file:///d:/kestrel/docs/PRD.md)
**Total tickets:** 39 (Phase 1: 13, Phase 2: 8, Phase 3: 10, Phase 4: 8)
**Convention:** `P<phase>-T<ticket>` numbering. Each ticket is a single atomic unit of work.

---

## Phase 1: Core Pipeline + State Machine + Basic FIX

**Goal:** End-to-end order round-trip through the pipeline with a stub venue.

**Exit criteria:** Submit a NewOrderSingle via FIX → order traverses Inbound GW → OMS Core → Outbound GW → stub venue acks → Execution Report traverses back → client receives ack. All under ThreadSanitizer with zero races.

---

### P1-T01: CMake Project Scaffold + Testing Integration

**Description:**
Set up the top-level CMake project structure for the Kestrel monorepo. Configure two build targets (Process A: OMS System, Process B: Venue Simulator). Integrate Google Test and Google Benchmark as dependencies (FetchContent or git submodule). Set up C++20 standard, compiler warnings (`-Wall -Wextra -Wpedantic`), and sanitizer build configurations (TSan, ASan). Verify with a trivial "hello world" test that `cmake --build . && ctest` succeeds.

**Requirements traced:** NFR-031, NFR-040, NFR-041

**Acceptance criteria:**
- [ ] `cmake --build .` succeeds with GCC 12+ / Clang 15+ in C++20 mode
- [ ] `ctest` runs and passes a trivial gtest placeholder
- [ ] Sanitizer build configs (`-DCMAKE_BUILD_TYPE=TSan`, `-DCMAKE_BUILD_TYPE=ASan`) compile without errors
- [ ] Directory structure has clear separation: `src/`, `include/`, `tests/`, `benchmarks/`
- [ ] Builds on both WSL2 and native Ubuntu without modification

**Key files/components:**
- `CMakeLists.txt` (root)
- `src/CMakeLists.txt`, `tests/CMakeLists.txt`, `benchmarks/CMakeLists.txt`

**Dependencies:** None (first ticket)

---

### P1-T02: Wire Event Structs

**Description:**
Define the fixed-size, trivially-copyable POD wire structs that flow through ring buffers between pipeline stages. These are the hot-path data carriers — no pointers, no `std::string`, no heap. Include all supporting enums (`Side`, `OrderType`, `TimeInForce`, `ExecType`, `OrdStatus`).

Structs to implement:
- `OrderEventWire` — new order from client (20-char ClOrdID, 12-char symbol, side, type, price, qty, session_id, mono_ts_ns)
- `CancelEventWire` — cancel/replace request (ClOrdID, OrigClOrdID, session_id, mono_ts_ns)
- `ExecReportEventWire` — execution report (exec_id, order_id, exec_type, ord_status, last_qty, last_px, cum_qty, leaves_qty, mono_ts_ns)
- `MarketDataEventWire` — normalized tick (symbol, bid/ask/last_px, mono_ts_ns)

**Requirements traced:** API_DESIGN §2.1, NFR-003

**Acceptance criteria:**
- [ ] All structs are `static_assert`-verified as trivially copyable and standard layout
- [ ] No `std::string`, no pointers, no virtual functions in any wire struct
- [ ] All char fields are fixed-size arrays (ClOrdID: `char[20]`, symbol: `char[12]`)
- [ ] Enums use `uint8_t` underlying type with a `Count` sentinel
- [ ] Unit test verifies `sizeof` of each struct matches expected layout

**Key files/components:**
- `include/kestrel/wire/wire_structs.h`
- `include/kestrel/wire/enums.h`
- `tests/wire/wire_structs_test.cpp`

**Dependencies:** P1-T01

---

### P1-T03: SPSC Ring Buffer Implementation

**Description:**
Implement the `SpscRingBuffer<T, Capacity>` template — the lock-free, single-producer single-consumer ring buffer that connects every pipeline stage. Must be cache-line-padded to prevent false sharing between head (producer) and tail (consumer) atomics. Use `acquire`/`release` memory ordering (not `seq_cst`). Power-of-2 capacity enforced via `static_assert` with bitmask indexing.

**Requirements traced:** NFR-010, API_DESIGN §2.2, LOW_LEVEL_DESIGN §1

**Acceptance criteria:**
- [ ] `static_assert` enforces power-of-2 capacity
- [ ] `head_` and `tail_` atomics are on separate cache lines (`alignas(64)`)
- [ ] `try_push` returns `false` when full (no blocking, no allocation)
- [ ] `try_pop` returns `false` when empty
- [ ] `size_approx()` provides monitoring-grade count (relaxed ordering acceptable)
- [ ] Write-then-publish ordering: payload written before index advanced
- [ ] Unit tests cover: empty pop, full push, single item, full capacity round-trip, wraparound correctness

**Key files/components:**
- `include/kestrel/core/spsc_ring_buffer.h`
- `tests/core/spsc_ring_buffer_test.cpp`

**Dependencies:** P1-T01

---

### P1-T04: SPSC Ring Buffer ThreadSanitizer Stress Tests

**Description:**
Write a dedicated stress test that runs 1M+ messages through the `SpscRingBuffer` under ThreadSanitizer. Producer and consumer run on separate `std::thread`s. Verify zero data races reported. Test both at-capacity operation (producer occasionally sees full) and at-empty operation (consumer occasionally sees empty).

**Requirements traced:** NFR-010

**Acceptance criteria:**
- [ ] Stress test pushes ≥1M messages through buffer with concurrent producer/consumer threads
- [ ] TSan build (`-fsanitize=thread`) reports zero data races
- [ ] Consumer receives all messages in order, with correct content
- [ ] Test exercises both "full" backpressure and "empty" drain scenarios
- [ ] Test completes within reasonable time (<30s)

**Key files/components:**
- `tests/core/spsc_ring_buffer_stress_test.cpp`

**Dependencies:** P1-T03

---

### P1-T05: OrderState and OrderEvent Enums

**Description:**
Define the `OrderState` and `OrderEvent` enums that drive the order state machine. Both must have a `Count` sentinel for exhaustive iteration in tests. Include utility functions: `to_string()` for logging, `is_terminal()` for state queries.

States: `PendingNew`, `New`, `PartiallyFilled`, `Filled`, `PendingCancel`, `Cancelled`, `PendingReplace`, `Replaced`, `Rejected`, `Expired`, `DoneForDay`

Events: `Ack`, `PartialFill`, `FullFill`, `CancelRequest`, `CancelAck`, `ReplaceRequest`, `ReplaceAck`, `RejectEvent`, `ExpireEvent`

**Requirements traced:** FR-011, FR-012

**Acceptance criteria:**
- [ ] Both enums have `Count` sentinel as the last value
- [ ] `to_string()` implemented for both enums (no missing cases)
- [ ] `is_terminal()` correctly identifies `Filled`, `Cancelled`, `Replaced`, `Rejected`, `Expired`, `DoneForDay`
- [ ] Underlying type is `uint8_t`
- [ ] Enums are in a shared header usable by both wire structs and domain logic

**Key files/components:**
- `include/kestrel/oms/order_state.h`
- `tests/oms/order_state_test.cpp`

**Dependencies:** P1-T01

---

### P1-T06: Table-Driven Order State Machine

**Description:**
Implement the order state machine as a `constexpr` 2D lookup table indexed by `[OrderState][OrderEvent]`. Default every cell to `OrderState::Rejected` (illegal transition), then explicitly populate the legal transitions. The transition function takes current state + event, returns the new state. This is the heart of OMS Core correctness.

Legal transitions to encode (as per PRD §6.2 / LOW_LEVEL_DESIGN §2):
- `PendingNew + Ack → New`
- `PendingNew + RejectEvent → Rejected`
- `New + PartialFill → PartiallyFilled`
- `New + FullFill → Filled`
- `New + CancelRequest → PendingCancel`
- `New + ReplaceRequest → PendingReplace`
- `New + RejectEvent → Rejected`
- `New + ExpireEvent → Expired`
- `PartiallyFilled + PartialFill → PartiallyFilled`
- `PartiallyFilled + FullFill → Filled`
- `PartiallyFilled + CancelRequest → PendingCancel`
- `PendingCancel + CancelAck → Cancelled`
- `PendingCancel + RejectEvent → New` (cancel rejected, revert)
- `PendingCancel + PartialFill → PendingCancel` (fill while pending cancel)
- `PendingCancel + FullFill → Filled` (filled before cancel processed)
- `PendingReplace + ReplaceAck → Replaced`
- `PendingReplace + RejectEvent → New` (replace rejected, revert)

**Requirements traced:** FR-011, FR-012, NFR-011, LOW_LEVEL_DESIGN §2

**Acceptance criteria:**
- [ ] Transition table is `constexpr` — evaluated at compile time
- [ ] Every undefined `(state, event)` pair defaults to `Rejected`
- [ ] `transition(state, event)` function returns new state in O(1) — single array lookup
- [ ] All legal transitions listed above are correctly encoded
- [ ] Table dimensions are `OrderState::Count × OrderEvent::Count`

**Key files/components:**
- `include/kestrel/oms/order_state_machine.h`
- `src/oms/order_state_machine.cpp` (if needed)

**Dependencies:** P1-T05

---

### P1-T07: Exhaustive State Machine Unit Tests

**Description:**
Write a unit test that programmatically enumerates every `(OrderState, OrderEvent)` pair — all `OrderState::Count × OrderEvent::Count` combinations — and asserts each one has an explicitly defined outcome (either a legal transition to a documented state, or `Rejected`). No unhandled cases. This test is the enforcement mechanism for FR-011/FR-012.

**Requirements traced:** FR-011, FR-012, NFR-011

**Acceptance criteria:**
- [ ] Test iterates all `(state, event)` pairs using `Count` sentinels — zero manual enumeration
- [ ] Every pair resolves to a legal transition or `Rejected` — no undefined behavior
- [ ] Test verifies each legal transition matches the documented transition table
- [ ] Test verifies that transitions from terminal states (Filled, Cancelled, etc.) are all `Rejected`
- [ ] Test count equals `OrderState::Count × OrderEvent::Count` (logged/asserted)

**Key files/components:**
- `tests/oms/order_state_machine_test.cpp`

**Dependencies:** P1-T06

---

### P1-T08: OrderPool — Pre-Allocated Order Storage

**Description:**
Implement `OrderPool` — a pre-allocated, fixed-capacity pool for `Order` domain objects. `order_id` IS the pool index (O(1) lookup, no hashing). Pool is reserved once at startup, never resized. Pool exhaustion is a defined failure mode (returns `std::nullopt`), not an OOM crash. Include `ClOrdID → order_id` lookup map for cancel/replace chain matching (FR-013). Also define the `Order` domain struct itself (richer than `OrderEventWire` — owns state, cum_qty, leaves_qty, avg_px, timestamps).

**Requirements traced:** NFR-003, FR-013, FR-014, LOW_LEVEL_DESIGN §3

**Acceptance criteria:**
- [ ] `OrderPool` reserves capacity once at construction — no runtime heap allocation for order creation
- [ ] `allocate()` returns `std::optional<uint64_t>` — `nullopt` on pool exhaustion
- [ ] `get(order_id)` returns `Order&` in O(1) via direct index
- [ ] `Order` domain struct has: order_id, cl_ord_id, orig_cl_ord_id, symbol, side, order_type, price, quantity, state, cum_qty, leaves_qty, avg_px, timestamps
- [ ] Invariant `cum_qty + leaves_qty == quantity` enforced/assertable after every fill
- [ ] ClOrdID lookup map supports duplicate detection (FR-014)
- [ ] Unit tests cover: allocate, get, pool exhaustion, duplicate ClOrdID rejection

**Key files/components:**
- `include/kestrel/oms/order.h`
- `include/kestrel/oms/order_pool.h`
- `src/oms/order_pool.cpp`
- `tests/oms/order_pool_test.cpp`

**Dependencies:** P1-T05

---

### P1-T09: Per-Thread Latency Histogram Infrastructure

**Description:**
Implement an HDR-histogram-style latency recorder suitable for hot-path use. Each pinned thread owns its own histogram instance (no cross-thread contention). Recording a sample is a single array increment — no allocation, no lock, no syscall. Reporting (p50/p99/p99.9/p99.99 computation) happens on a cold path reading a snapshot. Log-linear bucketing: fine-grained at low latencies, coarser at high.

**Requirements traced:** NFR-001, LOW_LEVEL_DESIGN §6

**Acceptance criteria:**
- [ ] `record(uint64_t latency_ns)` is O(1) — single array index + increment
- [ ] No heap allocation in `record()`
- [ ] Bucket index computation is branch-free or minimal-branch
- [ ] `percentile(double p)` returns the latency value at the given percentile
- [ ] Supports p50, p99, p99.9, p99.99 queries
- [ ] Unit test: record known values → verify percentile outputs are correct
- [ ] Thread-local ownership model — no mutexes, no atomics in the recording path

**Key files/components:**
- `include/kestrel/perf/latency_histogram.h`
- `src/perf/latency_histogram.cpp`
- `tests/perf/latency_histogram_test.cpp`

**Dependencies:** P1-T01

---

### P1-T10: Inbound Gateway — FIX Application + Wire Translation

**Description:**
Implement `InboundGatewayApplication` inheriting from `FIX::Application`. On `fromApp`: validate incoming FIX message (FR-003), translate `NewOrderSingle` (35=D) → `OrderEventWire`, `OrderCancelRequest` (35=F) → `CancelEventWire`, and `try_push` onto the inbound ring buffer. On the send side: drain the outbound exec report ring buffer and send `ExecutionReport` (35=8) FIX messages back to the client. Gateway must not block — QuickFIX calls `fromApp` on its own network thread.

Handle Logon/Logout/Heartbeat session lifecycle callbacks (FR-001).

**Requirements traced:** FR-001, FR-003, FR-010, API_DESIGN §4

**Acceptance criteria:**
- [ ] `fromApp` translates 35=D into `OrderEventWire` and pushes to ring buffer
- [ ] `fromApp` translates 35=F into `CancelEventWire` and pushes to ring buffer
- [ ] Invalid/malformed FIX messages are rejected with logged reason, no crash, no session corruption
- [ ] `fromApp` does NOT block — no OMS logic, no disk I/O, no allocations
- [ ] Outbound drain loop translates `ExecReportEventWire` back to FIX 35=8
- [ ] Logon/Logout/Heartbeat callbacks update session state only
- [ ] Timestamp (`mono_ts_ns`) captured at start of `fromApp`
- [ ] QuickFIX FIX 4.4 data dictionary configured

**Key files/components:**
- `include/kestrel/gateway/inbound_gateway.h`
- `src/gateway/inbound_gateway.cpp`
- `config/FIX44.xml` (QuickFIX data dictionary)
- `tests/gateway/inbound_gateway_test.cpp`

**Dependencies:** P1-T02, P1-T03

---

### P1-T11: Outbound Gateway — Wire Translation + Stub Venue

**Description:**
Implement `OutboundGatewayApplication` inheriting from `FIX::Application`. Send side: drain the OMS Core's outbound routing ring buffer, translate `OrderEventWire` → FIX `NewOrderSingle` (35=D) and `CancelEventWire` → FIX `OrderCancelRequest` (35=F), send to venue. Receive side: on `fromApp`, translate venue's incoming `ExecutionReport` (35=8) → `ExecReportEventWire`, push onto OMS Core's inbound exec report ring buffer.

Also implement a minimal **stub venue** FIX acceptor that immediately acks every order with `ExecType::New` and then `ExecType::Fill` — just enough to close the round-trip loop for Phase 1 exit criteria.

**Requirements traced:** FR-001, FR-030 (stub only), API_DESIGN §4

**Acceptance criteria:**
- [ ] Outbound gateway drains routing buffer and sends FIX orders to venue
- [ ] Outbound gateway receives FIX execution reports and pushes `ExecReportEventWire` to OMS Core buffer
- [ ] Stub venue accepts FIX logon, receives orders, immediately responds with New ack + Fill
- [ ] No blocking in `fromApp` — translate and `try_push` only
- [ ] Timestamp captured at start of each `fromApp`
- [ ] Stub venue runs as a separate `FIX::Application` (eventually separate process in Phase 2)

**Key files/components:**
- `include/kestrel/gateway/outbound_gateway.h`
- `src/gateway/outbound_gateway.cpp`
- `src/venue/stub_venue.h` / `src/venue/stub_venue.cpp`
- `tests/gateway/outbound_gateway_test.cpp`

**Dependencies:** P1-T02, P1-T03

---

### P1-T12: OMS Core Thread — Order Processing Pipeline

**Description:**
Implement the OMS Core as a single-threaded processing loop. It consumes `OrderEventWire` / `CancelEventWire` from the inbound ring buffer and `ExecReportEventWire` from the outbound gateway's ring buffer. For new orders: allocate from `OrderPool`, run state machine transition (`PendingNew`), push to outbound routing buffer. For execution reports from venue: apply state transition, update order fields (cum_qty, leaves_qty, avg_px), produce `ExecReportEventWire` for the client (push to inbound gateway's send buffer). For cancel requests: match via OrigClOrdID, transition to `PendingCancel`, forward to venue.

This is the single-threaded heart of the system (FR-016: emit exec report for every state transition, not just fills).

**Requirements traced:** FR-010, FR-013, FR-014, FR-016, NFR-013

**Acceptance criteria:**
- [ ] OMS Core runs as a single pinned thread with a drain loop
- [ ] New orders: allocate from pool → `PendingNew` → push to outbound buffer
- [ ] Venue acks/fills: apply state transition → update order → push exec report to inbound send buffer
- [ ] Cancel requests: match OrigClOrdID → transition → forward to venue or reject
- [ ] Duplicate ClOrdID rejected with reason (FR-014)
- [ ] Execution report generated for EVERY state transition, not just fills (FR-016)
- [ ] No order silently dropped — every inbound message results in a transition or explicit rejection (NFR-013)
- [ ] Latency histogram sample recorded at key checkpoints

**Key files/components:**
- `include/kestrel/oms/oms_core.h`
- `src/oms/oms_core.cpp`
- `tests/oms/oms_core_test.cpp`

**Dependencies:** P1-T03, P1-T06, P1-T08, P1-T09

---

### P1-T13: End-to-End Integration Test (Phase 1 Exit Criteria)

**Description:**
Write an integration test that wires together the full Phase 1 pipeline: Inbound Gateway → OMS Core → Outbound Gateway → Stub Venue → back. Submit a `NewOrderSingle` via a FIX initiator → verify the order traverses the full pipeline → receive the execution report ack back at the client. Run under ThreadSanitizer with zero races. This test validates the Phase 1 exit criteria.

**Requirements traced:** Phase 1 exit criteria, NFR-010

**Acceptance criteria:**
- [ ] Client submits `NewOrderSingle` via FIX → receives `ExecutionReport` (New) back
- [ ] Order traverses: Inbound GW → ring buffer → OMS Core → ring buffer → Outbound GW → FIX → Stub Venue → FIX → Outbound GW → ring buffer → OMS Core → ring buffer → Inbound GW → FIX → Client
- [ ] Full round-trip completes without errors, hangs, or crashes
- [ ] ThreadSanitizer reports zero data races
- [ ] Test logs show correct state transitions: `PendingNew → New → Filled`
- [ ] Execution report fields (ExecType, OrdStatus, cum_qty, leaves_qty) are correct
- [ ] Test is automated and runnable via `ctest`

**Key files/components:**
- `tests/integration/phase1_e2e_test.cpp`

**Dependencies:** P1-T10, P1-T11, P1-T12

---
---

## Phase 2: Venue Simulator + Market Data

**Goal:** Replace stub venue with a realistic, market-data-driven simulated venue.

**Exit criteria:** Full end-to-end order lifecycle with venue filling at live Binance prices. Kill and restart WebSocket → venue pauses → reconnects → resumes without stale price fills.

---

### P2-T01: Binance WebSocket Market Data Ingester

**Description:**
Implement `BinanceMarketDataFeed` conforming to `IMarketDataFeed`. Connect to `wss://stream.binance.com:9443/ws` and subscribe to BTC/USDT and ETH/USDT trade streams. Parse incoming JSON trade messages, extract price/quantity fields. The ingester runs on its own thread (not a pinned pipeline thread). Callback translates ticks and `try_push`es onto the market data ring buffer.

**Requirements traced:** FR-040, API_DESIGN §3.2

**Acceptance criteria:**
- [ ] Connects to Binance public WebSocket (no API key required)
- [ ] Subscribes to `btcusdt@trade` and `ethusdt@trade` streams
- [ ] Parses JSON trade messages and extracts price, quantity, timestamp
- [ ] Callback pushes `MarketDataEventWire` onto ring buffer
- [ ] Callback does NOT block — translate and `try_push` only
- [ ] Connection established and first tick received within 10s of startup
- [ ] Ingester thread is separate from pipeline threads

**Key files/components:**
- `include/kestrel/marketdata/i_market_data_feed.h`
- `include/kestrel/marketdata/binance_feed.h`
- `src/marketdata/binance_feed.cpp`
- `tests/marketdata/binance_feed_test.cpp`

**Dependencies:** P1-T02, P1-T03

---

### P2-T02: MarketDataEventWire Normalization Pipeline

**Description:**
Implement the normalization layer that translates raw Binance trade data into `MarketDataEventWire` structs. Map Binance symbol names to internal instrument identifiers. Maintain a simple best-bid/best-ask model from trade stream (last trade price used as reference). Push normalized ticks through the ring buffer to the Venue Core.

**Requirements traced:** FR-031, FR-040

**Acceptance criteria:**
- [ ] Binance symbol `BTCUSDT` maps to internal `BTC/USDT`, `ETHUSDT` to `ETH/USDT`
- [ ] `MarketDataEventWire` populated with bid_px, ask_px, last_px, mono_ts_ns
- [ ] Source timestamp from Binance preserved alongside receipt timestamp
- [ ] Normalized ticks flow through ring buffer to Venue Core consumer
- [ ] Unit test with mock feed data verifies correct normalization

**Key files/components:**
- `src/marketdata/tick_normalizer.h` / `src/marketdata/tick_normalizer.cpp`
- `tests/marketdata/tick_normalizer_test.cpp`

**Dependencies:** P2-T01

---

### P2-T03: WebSocket Disconnect/Reconnect Handling

**Description:**
Implement graceful disconnect/reconnect logic for the Binance WebSocket feed. On disconnect: venue must pause filling (no fills against stale price). On reconnect: update reference price from first fresh tick before resuming fills. Exponential backoff on repeated connection failures. Log all disconnect/reconnect events with timestamps.

**Requirements traced:** FR-041

**Acceptance criteria:**
- [ ] On WebSocket disconnect: venue receives a "stale" signal and stops filling
- [ ] On reconnect: first fresh tick updates reference price, then venue resumes
- [ ] No execution report ever uses a stale price (price from before disconnect)
- [ ] Exponential backoff on repeated failures (1s, 2s, 4s, ... capped)
- [ ] All disconnect/reconnect events logged with monotonic timestamps
- [ ] Test: simulate disconnect → verify venue pauses → simulate reconnect → verify venue resumes

**Key files/components:**
- `src/marketdata/binance_feed.cpp` (reconnect logic)
- `tests/marketdata/reconnect_test.cpp`

**Dependencies:** P2-T01

---

### P2-T04: Venue Core — Immediate Full Fill Model

**Description:**
Implement the Venue Core processing loop for Process B. Consumes `MarketDataEventWire` from the market data ring buffer to maintain a reference price per symbol. Consumes inbound orders (received via FIX from the Outbound Gateway), applies the v1 fill model: immediate full fill at current reference price. Produces `ExecutionReport` FIX messages (New ack, then Fill) back to the OMS.

**Requirements traced:** FR-030, FR-031

**Acceptance criteria:**
- [ ] Venue maintains reference price per symbol, updated on each market data tick
- [ ] On receiving an order: immediately sends `ExecType::New` ack
- [ ] Then immediately sends `ExecType::Fill` at current reference price with full quantity
- [ ] Fill price tracks live Binance price (within one tick update delay)
- [ ] If no reference price available (pre-first-tick or stale), order is rejected (not filled at zero)
- [ ] Venue Core runs as a single thread in Process B

**Key files/components:**
- `include/kestrel/venue/venue_core.h`
- `src/venue/venue_core.cpp`
- `tests/venue/venue_core_test.cpp`

**Dependencies:** P2-T02

---

### P2-T05: Venue FIX Acceptor (Process B)

**Description:**
Implement the venue-side FIX acceptor as a `FIX::Application` in Process B. Accept FIX sessions from Process A's Outbound Gateway. Receive `NewOrderSingle` (35=D), `OrderCancelRequest` (35=F), `OrderCancelReplaceRequest` (35=G) messages. Pass to Venue Core for processing. Send back `ExecutionReport` (35=8) responses. Handle FIX session lifecycle (Logon/Logout/Heartbeat).

**Requirements traced:** FR-030, FR-033

**Acceptance criteria:**
- [ ] FIX acceptor listens on configurable port, accepts session from OMS Outbound Gateway
- [ ] Translates incoming FIX 35=D → internal order representation for Venue Core
- [ ] Translates Venue Core's fill decisions → FIX 35=8 ExecutionReport sent to OMS
- [ ] Handles 35=F (cancel) and 35=G (replace) messages
- [ ] FIX session lifecycle (Logon/Logout/Heartbeat) works correctly
- [ ] Venue is swappable — no `IVenue` C++ interface, FIX protocol is the abstraction (FR-033)

**Key files/components:**
- `include/kestrel/venue/venue_fix_acceptor.h`
- `src/venue/venue_fix_acceptor.cpp`
- `config/venue_fix.cfg` (QuickFIX acceptor config)

**Dependencies:** P2-T04

---

### P2-T06: Two-Process Startup/Shutdown Orchestration

**Description:**
Implement the startup and shutdown sequencing for Process A (OMS System) and Process B (Venue Simulator) as defined in HIGH_LEVEL_ARCHITECTURE §4.

**Startup:** Process B starts first → Market Data Ingester connects → waits for at least one tick → Venue FIX acceptor opens. Then Process A starts → constructs buffers → Outbound Gateway connects to venue → Inbound Gateway opens last (no accepting client orders until venue path is confirmed live).

**Shutdown:** Reverse order, drain-before-close. Inbound GW stops accepting → drains in-flight orders → Outbound GW drains → FIX sessions close → Capture writer drains last.

**Requirements traced:** HLA §4

**Acceptance criteria:**
- [ ] Process B must be running and have received ≥1 tick before Process A connects
- [ ] Process A's Inbound Gateway opens only after Outbound Gateway confirms venue session is live
- [ ] Shutdown drains all in-flight orders before closing FIX sessions
- [ ] Capture writer drains its buffer to disk before process exit
- [ ] No client orders accepted into a system that can't route them
- [ ] Start/stop scripts or orchestration documented in README

**Key files/components:**
- `src/process_a_main.cpp` (OMS System entry point)
- `src/process_b_main.cpp` (Venue Simulator entry point)
- `scripts/start.sh`, `scripts/stop.sh`

**Dependencies:** P2-T05, P1-T10, P1-T11, P1-T12

---

### P2-T07: Cancel/Replace Request Handling

**Description:**
Implement full Cancel (35=F) and Cancel/Replace (35=G) request handling in the OMS Core. Match requests to original orders via `OrigClOrdID → order_id` lookup in `OrderPool`. Validate the order is in a cancellable/replaceable state (via state machine). Forward valid requests to venue via Outbound Gateway. Process venue's cancel/replace ack or reject. Generate appropriate `ExecutionReport` for client: `CancelAck` → `Cancelled`, or `CancelReject` with reason.

**Requirements traced:** FR-013

**Acceptance criteria:**
- [ ] Cancel request matched to original order via OrigClOrdID chain
- [ ] Cancel on cancellable order (New, PartiallyFilled): transitions to `PendingCancel`
- [ ] Cancel on non-cancellable order (Filled, Cancelled, etc.): `CancelReject` with reason
- [ ] Replace request matched and validated similarly
- [ ] Venue ack updates state to `Cancelled` / `Replaced`
- [ ] Venue reject reverts state to previous (e.g., `PendingCancel → New`)
- [ ] Execution reports emitted for all transitions
- [ ] Unit tests cover: successful cancel, cancel on filled order, cancel on already-cancelled order

**Key files/components:**
- `src/oms/oms_core.cpp` (cancel/replace logic)
- `tests/oms/cancel_replace_test.cpp`

**Dependencies:** P1-T12, P1-T06

---

### P2-T08: Duplicate ClOrdID Detection and Rejection

**Description:**
Implement duplicate `ClOrdID` detection within a FIX session in the OMS Core. Maintain a `ClOrdID → order_id` map in `OrderPool`. On each new order, check if the ClOrdID already exists for the session. If duplicate: reject immediately with a clear reason code (`DuplicateClOrdID`), do not create an order or forward to venue. First order with that ClOrdID remains unaffected.

**Requirements traced:** FR-014

**Acceptance criteria:**
- [ ] First order with a ClOrdID is accepted normally
- [ ] Second order with the same ClOrdID in the same session is rejected
- [ ] Rejection includes reason code `DuplicateClOrdID`
- [ ] Original order is unaffected by the duplicate submission
- [ ] Rejection generates an `ExecutionReport` with `ExecType::Rejected`
- [ ] Unit test: submit two orders with same ClOrdID → second rejected → first unaffected

**Key files/components:**
- `src/oms/oms_core.cpp` (duplicate check in order acceptance path)
- `tests/oms/duplicate_clordid_test.cpp`

**Dependencies:** P1-T08, P1-T12

---
---

## Phase 3: Capture/Replay + Risk Checks

**Goal:** Complete the correctness and observability story.

**Exit criteria:** Capture a session with 10,000 orders → replay → byte-identical resulting order/position state. All risk rules fire with logged `rule_id` attribution. Sequence gap injection test passes.

---

### P3-T01: CaptureRecordWire Struct + Binary Log Format

**Description:**
Define the `CaptureRecordWire` struct and the flat binary capture log format. Each record: `[8B mono_ts_ns][4B session_id+direction][4B raw_fix_len][NB raw_fix]`. The struct must be suitable for sequential binary writes. Include serialization/deserialization functions for reading/writing individual records to/from a byte stream.

**Requirements traced:** FR-050, LOW_LEVEL_DESIGN §9

**Acceptance criteria:**
- [ ] `CaptureRecordWire` contains: mono_ts_ns (8B), session_id+direction (4B packed), raw_fix_len (4B), raw_fix (variable)
- [ ] Serialize function writes record to a byte buffer in the documented format
- [ ] Deserialize function reads record from a byte buffer
- [ ] Round-trip test: serialize → deserialize → compare original
- [ ] Format handles session-level messages (heartbeats, logon) not just order messages

**Key files/components:**
- `include/kestrel/capture/capture_record.h`
- `src/capture/capture_record.cpp`
- `tests/capture/capture_record_test.cpp`

**Dependencies:** P1-T02

---

### P3-T02: Capture Writer Thread

**Description:**
Implement the dedicated capture writer thread that drains a capture ring buffer and writes records to an append-only binary log file. Uses buffered I/O (not `fsync` per message) to keep disk I/O off the hot path. The capture ring buffer is an `SpscRingBuffer<CaptureRecordWire, N>` — producer is the gateway thread, consumer is the capture writer. Implement `FileCaptureSink` conforming to `ICaptureSink`.

**Requirements traced:** FR-050, NFR-022, API_DESIGN §3.3

**Acceptance criteria:**
- [ ] Capture writer runs as a separate thread — not on any hot-path thread
- [ ] Drains capture ring buffer in a loop, writes to binary log file
- [ ] Uses buffered I/O — no `fsync` per message
- [ ] Correctly handles buffer draining on shutdown (no truncated log)
- [ ] `FileCaptureSink` implements `ICaptureSink` interface
- [ ] `InMemoryCaptureSink` implemented for unit tests
- [ ] Unit test: push N records → verify file contains exactly N records

**Key files/components:**
- `include/kestrel/capture/i_capture_sink.h`
- `include/kestrel/capture/file_capture_sink.h`
- `src/capture/file_capture_sink.cpp`
- `include/kestrel/capture/in_memory_capture_sink.h`
- `tests/capture/capture_writer_test.cpp`

**Dependencies:** P3-T01, P1-T03

---

### P3-T03: Gateway Capture Integration

**Description:**
Integrate the capture system into both Inbound and Outbound Gateways. Every inbound and outbound FIX message (including session-level: Logon, Heartbeat, etc.) must be pushed as a `CaptureRecordWire` onto the capture ring buffer. Timestamp with `CLOCK_MONOTONIC` at point of capture. Mark direction (inbound/outbound) and session_id in the packed field.

**Requirements traced:** FR-050

**Acceptance criteria:**
- [ ] Every FIX message passing through Inbound Gateway is captured (both received and sent)
- [ ] Every FIX message passing through Outbound Gateway is captured (both received and sent)
- [ ] Session-level messages (Logon, Logout, Heartbeat, TestRequest) are captured
- [ ] Each capture record has correct mono_ts_ns, session_id, direction, raw FIX bytes
- [ ] Integration test: run a session with N orders → capture log contains ≥2N entries (orders + acks + fills + session messages)

**Key files/components:**
- `src/gateway/inbound_gateway.cpp` (capture push added)
- `src/gateway/outbound_gateway.cpp` (capture push added)
- `tests/capture/gateway_capture_integration_test.cpp`

**Dependencies:** P3-T02, P1-T10, P1-T11

---

### P3-T04: Deterministic Replay Engine

**Description:**
Implement `FileReplaySource` conforming to `IReplaySource`. Reads the binary capture log sequentially and replays captured FIX messages into the OMS pipeline. Replay must produce byte-identical resulting order state and position ledger as the original run. Replay drives messages at the recorded `mono_ts_ns` pacing (or as-fast-as-possible in benchmark mode — mode is a constructor param). Include comparison tooling to diff original vs replayed state.

**Requirements traced:** FR-051, NFR-030

**Acceptance criteria:**
- [ ] `FileReplaySource` reads binary capture log and emits records via `next()`
- [ ] `InMemoryReplaySource` implemented for unit tests
- [ ] Replayed session produces byte-identical order states as original
- [ ] Replayed session produces byte-identical position ledger as original
- [ ] Pacing mode: replay at recorded timestamps (real-time) or as-fast-as-possible
- [ ] Comparison tool: dump final state from both runs, diff, assert identical
- [ ] Test: capture session A → replay session A → compare → byte-identical

**Key files/components:**
- `include/kestrel/capture/i_replay_source.h`
- `include/kestrel/capture/file_replay_source.h`
- `src/capture/file_replay_source.cpp`
- `src/tools/replay_compare.cpp`
- `tests/capture/replay_determinism_test.cpp`

**Dependencies:** P3-T03

---

### P3-T05: Replay-as-Benchmark Harness Mode

**Description:**
Extend the replay engine to serve as a fixed, repeatable benchmark workload. In benchmark mode: replay as-fast-as-possible, record latency histograms for each pipeline stage during replay, produce a benchmark report. The same capture file replayed N times should produce stable latency histograms (p99.9 variance <20% across runs on bare metal).

**Requirements traced:** FR-052

**Acceptance criteria:**
- [ ] Benchmark mode replays capture file at maximum speed (no pacing delay)
- [ ] Latency histograms recorded during replay for each pipeline stage
- [ ] Report output includes p50/p99/p99.9/p99.99 for each stage
- [ ] Same capture file replayed 3+ times produces consistent results
- [ ] Report labels environment (WSL2-directional vs bare-metal)
- [ ] Google Benchmark integration for standardized output

**Key files/components:**
- `src/capture/file_replay_source.cpp` (benchmark mode)
- `benchmarks/replay_benchmark.cpp`

**Dependencies:** P3-T04, P1-T09

---

### P3-T06: Risk Rule — Max Notional Per Order

**Description:**
Implement the first `IRiskRule`: `MaxNotionalRule`. Rejects orders where `price × quantity` exceeds a configurable maximum notional value. On rejection: return `RiskDecision{passed=false, rule_id=<configured_id>}`. The rule_id must be logged and included in the rejection execution report.

**Requirements traced:** FR-020, FR-024

**Acceptance criteria:**
- [ ] `MaxNotionalRule` implements `IRiskRule::evaluate()`
- [ ] Configurable max notional threshold (set at construction)
- [ ] Order with notional ≤ max: passes
- [ ] Order with notional > max: rejected with `rule_id`
- [ ] `RiskDecision.rule_id` traces to the specific rule instance
- [ ] Rejection logged with rule attribution
- [ ] Unit test: configure max $10,000 → $15,000 order rejected → $5,000 order passes

**Key files/components:**
- `include/kestrel/risk/i_risk_rule.h`
- `include/kestrel/risk/max_notional_rule.h`
- `src/risk/max_notional_rule.cpp`
- `tests/risk/max_notional_rule_test.cpp`

**Dependencies:** P1-T02

---

### P3-T07: Risk Rule — Max Quantity Per Order

**Description:**
Implement `MaxQuantityRule` as an `IRiskRule`. Rejects orders where quantity exceeds a configurable maximum. Same `RiskDecision` / `rule_id` pattern as P3-T06.

**Requirements traced:** FR-021, FR-024

**Acceptance criteria:**
- [ ] `MaxQuantityRule` implements `IRiskRule::evaluate()`
- [ ] Configurable max quantity threshold
- [ ] Order with qty ≤ max: passes
- [ ] Order with qty > max: rejected with `rule_id`
- [ ] Rejection logged with rule attribution
- [ ] Unit test: configure max qty=100 → qty=150 rejected → qty=50 passes

**Key files/components:**
- `include/kestrel/risk/max_quantity_rule.h`
- `src/risk/max_quantity_rule.cpp`
- `tests/risk/max_quantity_rule_test.cpp`

**Dependencies:** P3-T06 (shares `IRiskRule` interface)

---

### P3-T08: Risk Rules — Fat-Finger Check + Rate Limiter

**Description:**
Implement two more `IRiskRule` implementations:

1. **FatFingerRule** (FR-022): Reject limit orders with price outside N% of the last traded price. Requires access to the current reference price per symbol.

2. **RateLimiterRule** (FR-023): Reject orders exceeding a configurable max orders/sec per session. Uses a sliding window or token bucket counter. Rejection reason clearly identifies rate-limiting.

Integrate all risk rules into OMS Core's `evaluate_all()` loop (sequential, fixed-order, short-circuit on first failure).

**Requirements traced:** FR-022, FR-023, FR-024, LOW_LEVEL_DESIGN §8

**Acceptance criteria:**
- [ ] `FatFingerRule`: configure 5%, last price $100 → $120 limit order rejected, $103 passes
- [ ] `FatFingerRule` receives current reference price (injected or queried)
- [ ] `RateLimiterRule`: configure 100/sec → 150 orders in 1s → first 100 accepted, remaining 50 rejected
- [ ] All risk rules integrated into OMS Core's `evaluate_all()` sequential loop
- [ ] Short-circuit: first failing rule stops evaluation
- [ ] Evaluation order is fixed and deterministic (required for replay correctness)
- [ ] Every rejection has `rule_id` in `RiskDecision` and logged

**Key files/components:**
- `include/kestrel/risk/fat_finger_rule.h`
- `include/kestrel/risk/rate_limiter_rule.h`
- `src/risk/fat_finger_rule.cpp`
- `src/risk/rate_limiter_rule.cpp`
- `src/oms/oms_core.cpp` (integrate risk evaluation)
- `tests/risk/fat_finger_rule_test.cpp`
- `tests/risk/rate_limiter_rule_test.cpp`

**Dependencies:** P3-T06, P1-T12

---

### P3-T09: In-Memory Position Ledger

**Description:**
Implement the in-memory position ledger in OMS Core. Array-indexed by `instrument_index` (not hash-mapped) — resolve `symbol → instrument_index` at startup, then `positions_[instrument_index]` for O(1) update on every fill. Position fields: net_qty, avg_price, realized_pnl, updated_at. Updated on every fill-type `ExecutionReport`. Queryable at any point.

Enforce invariant: `cum_qty + leaves_qty == quantity` on every order after every fill.

**Requirements traced:** FR-015, LOW_LEVEL_DESIGN §7

**Acceptance criteria:**
- [ ] Positions stored in `std::vector<Position>` indexed by `instrument_index`
- [ ] `symbol → instrument_index` map resolved once at startup
- [ ] Position updated on every fill (net_qty, avg_price, realized_pnl)
- [ ] No hashing on the fill path — direct array index
- [ ] `cum_qty + leaves_qty == quantity` invariant asserted after every fill
- [ ] Unit test: submit 1000 orders with fills → verify positions match fold of all execution reports

**Key files/components:**
- `include/kestrel/oms/position_ledger.h`
- `src/oms/position_ledger.cpp`
- `tests/oms/position_ledger_test.cpp`

**Dependencies:** P1-T12

---

### P3-T10: FIX Sequence Gap Handling + Session State Persistence

**Description:**
Implement FIX sequence number gap detection and recovery on both gateways. On detecting an incoming sequence gap: issue `ResendRequest`. On receiving `ResendRequest`: respond with `SequenceReset` (gap-fill mode). Process gap-fill responses and resume normal processing with zero order state corruption.

Also implement session state persistence (FR-005): persist `seq_in` / `seq_out` across restarts so a crash doesn't desynchronize the session. On restart, resume with correct sequence numbers — no resend storm.

**Requirements traced:** FR-002, FR-005

**Acceptance criteria:**
- [ ] Sequence gap detected → `ResendRequest` issued automatically
- [ ] `ResendRequest` received → `SequenceReset` (gap-fill) sent
- [ ] Gap-fill processed → normal message processing resumes
- [ ] Zero order state corruption after gap recovery (verified by pre/post state comparison)
- [ ] Session sequence numbers (`seq_in`, `seq_out`) persisted to file
- [ ] Kill and restart Process A → outbound gateway resumes with correct sequence numbers
- [ ] No resend storm on restart
- [ ] Test: inject artificial sequence gap → verify recovery → verify order state integrity

**Key files/components:**
- `src/gateway/inbound_gateway.cpp` (gap handling)
- `src/gateway/outbound_gateway.cpp` (gap handling)
- `src/gateway/session_state_store.h` / `src/gateway/session_state_store.cpp`
- `tests/gateway/sequence_gap_test.cpp`
- `tests/gateway/session_persistence_test.cpp`

**Dependencies:** P1-T10, P1-T11

---
---

## Phase 4: Benchmarking + Hardening

**Goal:** Measure, publish, and harden.

**Exit criteria:** Published benchmark report with honest p99.9 latency number on bare metal. ThreadSanitizer clean under sustained load. Fuzz testing: zero crashes over 10M iterations.

---

### P4-T01: Bare-Metal Benchmarking Environment Setup

**Description:**
Document and script the bare-metal Ubuntu benchmarking environment setup. Configure CPU isolation (`isolcpus`), disable frequency scaling (performance governor), disable hyperthreading if applicable, configure `SCHED_FIFO` for pinned threads. Document all environment parameters so results are reproducible. All latency figures from this environment labeled "bare-metal" per NFR-002.

**Requirements traced:** NFR-002, NFR-004

**Acceptance criteria:**
- [ ] Setup script configures: CPU isolation, performance governor, IRQ affinity
- [ ] Documentation lists exact hardware, kernel version, kernel parameters
- [ ] Thread pinning (`pthread_setaffinity_np`) verified for all pipeline threads
- [ ] Before/after comparison: WSL2-directional vs bare-metal numbers for the same workload
- [ ] All benchmark output includes environment tag (`WSL2-directional` or `bare-metal`)

**Key files/components:**
- `scripts/setup_benchmark_env.sh`
- `docs/BENCHMARK_ENVIRONMENT.md`

**Dependencies:** None (can be done independently)

---

### P4-T02: Full Latency Benchmark Suite

**Description:**
Implement a comprehensive latency benchmark suite using Google Benchmark. Measure stage-to-stage latency for each pipeline boundary: Inbound GW → OMS Core, OMS Core → Outbound GW, Outbound GW → Venue, Venue → Outbound GW → OMS Core → Inbound GW (full round-trip). Report p50/p99/p99.9/p99.99 from HDR histograms. Measure both single-order-in-flight and sustained-throughput scenarios.

**Requirements traced:** NFR-001, NFR-004

**Acceptance criteria:**
- [ ] Benchmarks for each stage-to-stage boundary
- [ ] Full end-to-end order-to-ack round-trip benchmark
- [ ] Single-order-in-flight (best case) and sustained-throughput (realistic) scenarios
- [ ] Output: p50/p99/p99.9/p99.99 latency histograms per stage
- [ ] Google Benchmark integration for standardized output
- [ ] Results reproducible across runs (p99.9 variance <20% on bare metal)
- [ ] Environment tag included in all output

**Key files/components:**
- `benchmarks/stage_latency_bench.cpp`
- `benchmarks/e2e_latency_bench.cpp`
- `benchmarks/throughput_bench.cpp`

**Dependencies:** P1-T09, P3-T05

---

### P4-T03: Zero-Heap-Allocation Verification

**Description:**
Verify that the hot path (order receipt → risk check → routing → fill → exec report back) performs zero heap allocations. Implement a custom allocator trap that intercepts `malloc`/`new` on hot-path threads and logs/traps any allocation. Run 10,000 orders through the pipeline with the trap enabled. Any allocation is a failing test.

**Requirements traced:** NFR-003

**Acceptance criteria:**
- [ ] Custom allocator hook intercepts `malloc`/`operator new` on pinned threads
- [ ] Run 10,000 orders through full pipeline with trap enabled
- [ ] Zero allocations recorded on hot-path threads
- [ ] Allocations on cold-path threads (capture writer, startup) are allowed and not flagged
- [ ] Test is automated and runnable via `ctest`
- [ ] Any hot-path allocation fails the test with stack trace

**Key files/components:**
- `tests/perf/zero_alloc_test.cpp`
- `src/perf/malloc_trap.h` / `src/perf/malloc_trap.cpp`

**Dependencies:** P1-T12, P3-T08

---

### P4-T04: FIX Parser Fuzz Testing

**Description:**
Implement a fuzz-testing harness for the FIX message parsing and validation path in both gateways. Feed malformed, truncated, oversized, and adversarial FIX byte sequences into the `fromApp` translation path. Verify zero crashes, zero hangs, zero session state corruption. Use libFuzzer or AFL-style fuzzing.

**Requirements traced:** NFR-012, FR-003

**Acceptance criteria:**
- [ ] Fuzz harness feeds random/malformed bytes to FIX parsing path
- [ ] 10M+ iterations with zero crashes
- [ ] Zero hangs (timeout detection)
- [ ] Zero session state corruption (session remains usable after malformed input)
- [ ] Coverage-guided fuzzing (libFuzzer or AFL)
- [ ] Corpus of interesting inputs saved for regression testing

**Key files/components:**
- `tests/fuzz/fix_parser_fuzz.cpp`
- `tests/fuzz/corpus/` (seed inputs)

**Dependencies:** P1-T10

---

### P4-T05: Adversarial Venue Modes — Partial Fills + Delayed Acks

**Description:**
Extend the Venue Simulator (Process B) with configurable adversarial behaviors:
1. **Partial fills:** Fill a random fraction of the order quantity, requiring OMS to handle `PartiallyFilled` state and accumulate `cum_qty` across multiple fills.
2. **Delayed acks:** Introduce configurable latency before sending New ack or Fill, testing OMS behavior when orders are in `PendingNew` for extended periods.
3. **Random rejects:** Randomly reject a configurable percentage of orders.

Each mode independently activatable via config.

**Requirements traced:** FR-032

**Acceptance criteria:**
- [ ] Partial fill mode: order filled in 2-5 random chunks → OMS accumulates correctly → `cum_qty + leaves_qty == quantity` after each
- [ ] Delayed ack mode: configurable delay (e.g., 100ms-1s) before ack/fill
- [ ] Random reject mode: configurable reject rate (e.g., 10%) → OMS handles Rejected state
- [ ] Each mode independently toggleable via venue config file
- [ ] OMS handles all modes without state corruption
- [ ] Unit tests for each adversarial mode

**Key files/components:**
- `src/venue/venue_core.cpp` (adversarial modes)
- `config/venue_adversarial.cfg`
- `tests/venue/adversarial_test.cpp`

**Dependencies:** P2-T04

---

### P4-T06: Adversarial Venue Modes — Out-of-Order Reports + Session Drops

**Description:**
Extend the Venue Simulator with additional adversarial behaviors:
1. **Out-of-order execution reports:** Send Fill before New ack, or send reports for different orders interleaved — test OMS's ability to handle non-sequential venue responses.
2. **Session drops mid-order:** Drop the FIX session while orders are in-flight (`PendingNew` or `PendingCancel`), forcing the OMS to handle orphaned orders and session reconnection.

**Requirements traced:** FR-032

**Acceptance criteria:**
- [ ] Out-of-order mode: Fill arrives before New ack → OMS handles without corruption
- [ ] Interleaved reports: reports for different orders arrive interleaved → each order state correct
- [ ] Session drop mode: FIX session dropped while order is PendingNew → OMS detects → handles reconnect
- [ ] Orphaned orders (submitted but no response due to drop) are handled with defined policy
- [ ] Each mode independently toggleable
- [ ] OMS state remains consistent after all adversarial scenarios

**Key files/components:**
- `src/venue/venue_core.cpp` (adversarial modes)
- `tests/venue/session_drop_test.cpp`
- `tests/venue/out_of_order_test.cpp`

**Dependencies:** P4-T05, P3-T10

---

### P4-T07: Latency Histogram Live Dump

**Description:**
Implement the ability to inspect latency histograms from a running system without halting it. Periodically (configurable interval, e.g., every 10s) swap out the current histogram and dump the snapshot to a log file or stdout. The swap must be safe: the recording thread writes to a fresh histogram while the reporting thread reads the old one. No locks on the recording path.

**Requirements traced:** NFR-021

**Acceptance criteria:**
- [ ] Periodic dump at configurable interval (default: 10s)
- [ ] Dump shows p50/p99/p99.9/p99.99 for each pipeline stage
- [ ] No locks or blocking on the hot-path recording thread during dump
- [ ] Histogram swap is atomic (double-buffer or similar)
- [ ] Output format: human-readable text with timestamp and stage labels
- [ ] System continues processing orders during and after dump

**Key files/components:**
- `src/perf/latency_histogram.cpp` (live dump extension)
- `src/perf/histogram_reporter.h` / `src/perf/histogram_reporter.cpp`
- `tests/perf/histogram_live_dump_test.cpp`

**Dependencies:** P1-T09

---

### P4-T08: CI Pipeline + Published Benchmark Report

**Description:**
Set up a GitHub Actions CI pipeline on the free tier. Pipeline runs: build (GCC + Clang), unit tests, ThreadSanitizer stress tests, and (optionally) a smoke benchmark. Produce a published benchmark report document with full methodology disclosure: hardware specs, kernel config, thread pinning, warm-up procedure, number of iterations, all percentiles. Include WSL2-directional numbers alongside bare-metal numbers. No cherry-picking — publish the real numbers.

**Requirements traced:** NFR-031, NFR-002, NFR-004

**Acceptance criteria:**
- [ ] GitHub Actions workflow: build → test → TSan stress → pass
- [ ] Workflow runs on free tier within time limits
- [ ] Workflow tests both GCC and Clang
- [ ] Benchmark report published as `docs/BENCHMARK_REPORT.md`
- [ ] Report includes: hardware, kernel, methodology, warm-up, iteration count
- [ ] Report includes p50/p99/p99.9/p99.99 for each stage + E2E
- [ ] WSL2 numbers labeled "directional", bare-metal labeled "representative"
- [ ] No cherry-picked numbers — actual measured values published

**Key files/components:**
- `.github/workflows/ci.yml`
- `docs/BENCHMARK_REPORT.md`

**Dependencies:** P4-T02, P4-T03, P4-T04

---
---

## Summary

| Phase | Tickets | Scope |
|-------|---------|-------|
| **Phase 1** | P1-T01 → P1-T13 (13) | Core pipeline, state machine, ring buffers, basic FIX, E2E integration |
| **Phase 2** | P2-T01 → P2-T08 (8) | Live venue simulator, Binance market data, cancel/replace, two-process |
| **Phase 3** | P3-T01 → P3-T10 (10) | Capture/replay, risk rules, position ledger, sequence gap handling |
| **Phase 4** | P4-T01 → P4-T08 (8) | Benchmarking, fuzz testing, adversarial modes, CI, published report |
| **Total** | **39 tickets** | |
