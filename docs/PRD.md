# Product Requirements Document — Kestrel

**Document ID:** PRD-001
**Version:** 1.0
**Status:** Draft
**Date:** 2026-09-09
**Author:** Niraj
**Traces to:** [PROJECT_CHARTER.md](file:///d:/kestrel/docs/PROJECT_CHARTER.md), [REQUIREMENTS.md](file:///d:/kestrel/docs/REQUIREMENTS.md), [CORE_ENTITIES.md](file:///d:/kestrel/docs/CORE_ENTITIES.md), [HIGH_LEVEL_ARCHITECTURE.md](file:///d:/kestrel/docs/HIGH_LEVEL_ARCHITECTURE.md), [API_DESIGN.md](file:///d:/kestrel/docs/API_DESIGN.md), [LOW_LEVEL_DESIGN.md](file:///d:/kestrel/docs/LOW_LEVEL_DESIGN.md)

---

## 1. Executive Summary

**Kestrel** is an institutional-grade Order Management System (OMS) core with inbound and outbound FIX protocol gateways, connected to a simulated FIX-speaking venue fed by real crypto market data. It is a solo-developer portfolio project that demonstrates p99.9-grade engineering rigor in correctness, protocol conformance, and measured latency — built on a $0 budget with no paid infrastructure.

The primary audience is hiring managers and senior engineers evaluating systems-level C++ skill. Every design decision, latency number, and trade-off in Kestrel is documented honestly — no cherry-picked benchmarks, no glossed-over limitations, no pretended precision about unmeasured quantities.

---

## 2. Problem Statement

Low-latency order management is the canonical "hard problem" in financial technology — it sits at the intersection of protocol correctness (FIX), concurrency (lock-free pipelines), performance engineering (sub-millisecond latency budgets), and operational rigor (deterministic replay, exhaustive state machine coverage). There is no credible way to demonstrate mastery of these skills with a toy project that skips the hard parts.

Kestrel exists to **not skip the hard parts**: real FIX over real TCP (not mocked), real market data (not synthetic noise), real lock-free ring buffers (not `std::mutex` wrappers), real latency histograms (not averages), and a real state machine with exhaustive transition coverage (not scattered `if`/`else` chains).

---

## 3. Target Audience

| Audience | What they evaluate |
|---|---|
| **Hiring managers** (HFT, sell-side, infra) | Systems-level C++ skill, understanding of FIX protocol, latency discipline, engineering rigor |
| **Senior engineers** reviewing the codebase | Correctness of lock-free code, state machine completeness, memory discipline (zero hot-path heap allocation), honest latency reporting |
| **The developer (Niraj)** | Deep learning of real-world OMS architecture, LMAX-style pipeline design, FIX protocol mechanics, and performance measurement methodology |

---

## 4. Product Overview

### 4.1 System Context

Kestrel is two OS processes on the same machine, connected over loopback TCP via a real FIX session:

```
┌─────────────────── Process A: OMS System ───────────────────┐
│                                                              │
│  [Inbound GW]  ──ring buf──▸  [OMS Core]  ──ring buf──▸  [Outbound GW]  │
│       ▲                         │    ▲                         │          │
│       │                    risk checks  │                      │          │
│   FIX session              position ledger                FIX session     │
│   (client-facing)                                        (venue-facing)   │
│                                                              │
│                  [Capture Writer Thread]                      │
│                  (drains capture ring buffers to disk)        │
└──────────────────────────────────────────────────────────────┘
              │ loopback TCP, real FIX 4.4
              ▼
┌─────────────────── Process B: Venue Simulator ──────────────┐
│                                                              │
│  [Market Data Ingester]  ──ring buf──▸  [Venue Core / FIX Acceptor]  │
│       │                                      │               │
│   Binance WebSocket                    fill model +          │
│   (BTC/USDT, ETH/USDT)               FIX session            │
│                                       (OMS-facing)           │
└──────────────────────────────────────────────────────────────┘
```

### 4.2 Why Two Processes

| Concern | Benefit of two-process architecture |
|---|---|
| **Realism** | The venue behaves like a real counterparty — actual TCP, actual FIX serialization/deserialization, actual kernel-mediated latency |
| **Resilience testing** | Kill Process B to genuinely test reconnect/sequence-gap handling (FR-002) — no faking |
| **Latency measurement** | Measures the full realistic path including the network hop — the number you'd actually defend in an interview |

### 4.3 Concurrency Model

LMAX Disruptor-style pipeline: pinned threads per stage, connected by lock-free SPSC ring buffers. No mutexes on the hot path.

**Process A threads:**
1. **Inbound Gateway** — owns client-facing FIX session, translates FIX→wire structs, pushes onto OMS Core buffer, drains outbound exec report buffer back to client
2. **OMS Core** — single-threaded heart: order state machine, risk checks, position ledger
3. **Outbound Gateway** — owns venue-facing FIX session, translates wire structs→FIX for outbound, translates FIX→wire structs for inbound exec reports
4. **Capture Writer** — drains capture ring buffer to append-only binary log file (disk I/O kept off the hot path)

**Process B threads:**
1. **Market Data Ingester** — WebSocket connection to Binance, normalizes ticks into wire structs
2. **Venue Core / FIX Acceptor** — maintains reference price, applies fill model, emits execution reports

---

## 5. Resolved Design Decisions

The following open questions from the existing docs have been resolved for this PRD:

| Decision | Resolution | Source |
|---|---|---|
| **FIX version** | FIX 4.4 for v1; architecture must not block a later 5.0 SP2 upgrade (FR-004) | REQUIREMENTS.md §5 |
| **Position/ledger persistence** | In-memory only for v1; rebuild from execution reports on restart | REQUIREMENTS.md §5 |
| **Venue fill model** | Immediate full fill only for v1; probabilistic partial fills and latency-delayed fills added in later iterations | REQUIREMENTS.md §5 |
| **Market data source** | Binance public WebSocket — BTC/USDT and ETH/USDT pairs | FR-040/FR-031 |
| **Ring buffer topology** | Per-type ring buffers (one per wire struct type per stage boundary); revisit after benchmarking | API_DESIGN.md §2.1 |
| **Product name** | Kestrel | — |
| **Order types** | Market and Limit only | — |
| **Sides** | Buy and Sell only | — |
| **Time-in-force** | Day and GTC only | — |
| **Build system** | CMake | — |
| **C++ standard** | C++20 | — |
| **Testing framework** | Google Test (gtest) + Google Benchmark | — |

---

## 6. Functional Requirements

### 6.1 FIX Session Management

| ID | Requirement | Priority | Acceptance Criteria |
|---|---|---|---|
| FR-001 | Establish and maintain FIX 4.4 sessions (Logon/Logout/Heartbeat/TestRequest) on both inbound and outbound gateways via QuickFIX/C++. | Must | Both sessions complete Logon handshake and maintain heartbeat for >60s without session drops. |
| FR-002 | Detect sequence number gaps and issue/respond to ResendRequest and SequenceReset (gap-fill). | Must | Inject artificial sequence gap in test harness → system issues ResendRequest → processes gap-fill response → resumes normal processing with zero order state corruption (verified by pre/post order state snapshots). |
| FR-003 | Reject and log any message failing FIX tag/value syntax validation without crashing or corrupting session state. | Must | Send 100 malformed FIX messages → all rejected with logged reason → session continues normally → no segfaults or undefined behavior under ThreadSanitizer. |
| FR-004 | Support configurable FIX dictionary version (v1: FIX 4.4). Architecture must not hardcode assumptions that block a later 5.0 SP2 upgrade. | Should | No FIX version constants embedded in OMS Core logic; all version-specific behavior confined to gateway translation layer. |
| FR-005 | Persist session state (sequence numbers) across restarts. | Should | Kill and restart Process A → outbound gateway resumes with correct sequence numbers → no resend storm. |

### 6.2 Order Management (OMS Core)

| ID | Requirement | Priority | Acceptance Criteria |
|---|---|---|---|
| FR-010 | Accept New Order Single (35=D) and create order in state `PendingNew`. | Must | Submit Market order for BTC/USDT → order created in `PendingNew` → `OrderEventWire` pushed to OMS Core buffer within one ring buffer drain cycle. |
| FR-011 | Implement full order state machine as an explicit, exhaustively-tested transition table. States: `PendingNew → New → {PartiallyFilled, Filled, PendingCancel, PendingReplace, Rejected, Expired, DoneForDay}` with `Cancelled`/`Replaced` as terminal states from Pending* states. | Must | Unit test enumerates all `(OrderState::Count × OrderEvent::Count)` = every pair → each has either a defined legal transition or an explicit `Rejected` outcome → zero unhandled cases. |
| FR-012 | Reject any state transition not defined in the transition table (e.g., Cancel on Filled order). Generate appropriate Reject/CancelReject response. | Must | Attempt Cancel on Filled order → CancelReject with reason code → order remains Filled → rejection logged with rule attribution. |
| FR-013 | Support Order Cancel Request (35=F) and Order Cancel/Replace Request (35=G) via ClOrdID/OrigClOrdID chain matching. | Must | Submit order → submit cancel referencing OrigClOrdID → cancel processes against correct order → cancel ack/reject generated for correct order. |
| FR-014 | Reject duplicate ClOrdID values within a session. | Must | Submit two orders with same ClOrdID → second is rejected with duplicate reason → first order unaffected. |
| FR-015 | Maintain in-memory position ledger, updated on every fill, queryable at any point. Array-indexed by instrument (not hash-mapped). | Should | Submit 1000 orders with fills → `positions_[instrument_index]` matches the fold of all execution reports for that instrument → `cum_qty + leaves_qty == quantity` invariant holds on every order after every fill. |
| FR-016 | Generate Execution Reports (35=8) reflecting every state transition, not just fills. | Must | Order lifecycle: PendingNew→New→PartialFill→Fill → four distinct ExecReportEventWire structs emitted, each with correct ExecType and OrdStatus. |

**Supported order parameters for v1:**
- **Order types:** Market, Limit
- **Sides:** Buy, Sell
- **Time-in-force:** Day, GTC

### 6.3 Risk Checks (Pre-Trade)

| ID | Requirement | Priority | Acceptance Criteria |
|---|---|---|---|
| FR-020 | Reject orders exceeding configurable max notional value per order. | Must | Configure max notional = $10,000 → submit $15,000 order → rejected with `rule_id` tracing to max-notional rule → RiskDecision logged. |
| FR-021 | Reject orders exceeding configurable max quantity per order. | Must | Configure max qty = 100 → submit qty=150 → rejected with `rule_id` → logged. |
| FR-022 | Fat-finger check: reject orders with price outside N% of last traded price. | Should | Configure fat-finger = 5% → last price = $100 → submit limit order at $120 → rejected as fat-finger → `rule_id` logged. |
| FR-023 | Rate-limit inbound order messages per session (configurable max orders/sec). Reject excess with clear reason code. | Should | Configure limit = 100 orders/sec → submit 150 orders in 1 second → first 100 accepted, remaining 50 rejected with rate-limit reason. |
| FR-024 | All risk checks execute on the hot path pre-routing. Every rejection is attributable to a specific, logged `rule_id`. | Must | No risk rejection anywhere in the system without a `rule_id` in the RiskDecision struct and a corresponding log entry. |

**Risk rule evaluation:** Sequential, fixed-order, short-circuit on first failure. Deterministic for replay correctness (NFR-030).

### 6.4 Simulated Venue (Process B)

| ID | Requirement | Priority | Acceptance Criteria |
|---|---|---|---|
| FR-030 | Accept inbound FIX orders and respond with Execution Reports (New ack, Fill, Reject) per configured fill model. | Must | Submit Market Buy BTC/USDT → venue acks (New) → immediately fills at reference price → Execution Report sent back via FIX. |
| FR-031 | Reference price driven by live Binance public WebSocket (BTC/USDT, ETH/USDT). | Must | Venue's fill price tracks Binance live price within one tick update delay → verified by comparing venue fills against contemporaneous Binance ticks. |
| FR-032 | Support configurable adversarial behaviors: partial fills, delayed acks, rejects, out-of-order execution reports, session drops mid-order. | Should | (v2+) Each adversarial mode independently activatable via config → OMS handles each without state corruption. |
| FR-033 | Venue swappable behind the FIX session boundary — no `IVenue` C++ interface needed; the FIX protocol itself is the abstraction. | Could | Point Outbound Gateway at a different FIX acceptor (e.g., a second venue simulator with different fill model) → no code changes required. |

**V1 fill model:** Immediate full fill only. Venue receives order → immediately fills at current reference price → returns Execution Report.

### 6.5 Market Data Ingestion

| ID | Requirement | Priority | Acceptance Criteria |
|---|---|---|---|
| FR-040 | Connect to Binance public WebSocket and normalize ticks into `MarketDataEventWire`. | Must | Process B connects to `wss://stream.binance.com` → receives BTC/USDT trade stream → produces `MarketDataEventWire` with bid/ask/last price → pushed to ring buffer. |
| FR-041 | Handle feed disconnects/reconnects gracefully without propagating stale price into venue simulator. | Must | Kill WebSocket connection → venue pauses filling (no fills against stale price) → reconnect → venue resumes filling at fresh price. |
| FR-042 | Optionally translate normalized ticks into FIX MarketData messages (35=W/X). | Could | (deferred) |

### 6.6 Capture & Replay

| ID | Requirement | Priority | Acceptance Criteria |
|---|---|---|---|
| FR-050 | Capture every inbound and outbound FIX message with monotonic high-resolution timestamp to a binary log. | Must | Run a session with N orders → capture log contains exactly 2N+ entries (orders + acks + fills + session messages) → each entry has `mono_ts_ns`, `session_id`, direction, and raw FIX bytes. |
| FR-051 | Replay a captured session deterministically, producing byte-identical resulting order/position state. | Must | Capture session A → replay session A on a different run → compare resulting order states and position ledger → byte-identical. |
| FR-052 | Replay usable as a benchmark harness input (fixed, repeatable workload). | Should | Replay same capture file 10 times → latency histograms are stable (p99.9 variance < 20% across runs, bare metal). |

**Capture format:** Flat binary — `[8B mono_ts_ns][4B session_id+direction][4B raw_fix_len][NB raw_fix]`. Written by dedicated capture writer thread via buffered I/O (not `fsync` per message).

---

## 7. Non-Functional Requirements

### 7.1 Performance & Latency

| ID | Requirement | Priority | Acceptance Criteria |
|---|---|---|---|
| NFR-001 | Measure and report internal stage-to-stage latency as p50/p99/p99.9/p99.99 histograms. | Must | Latency histogram infrastructure is a first-class deliverable — built in Phase 1, not bolted on later. Each pinned thread owns its own histogram instance (no cross-thread contention). |
| NFR-002 | WSL2 latency figures labeled "directional" in all reports. Only bare-metal Linux figures labeled "representative." | Must | Every latency report includes environment tag (`WSL2-directional` or `bare-metal`). |
| NFR-003 | Hot path performs zero heap allocation — verified by instrumentation or allocator trap. | Should | Run 10,000 orders through the pipeline with a custom allocator that logs/traps any `malloc` on the hot-path threads → zero allocations recorded. |
| NFR-004 | Aspirational target: end-to-end order-to-ack p99.9 < 1ms on bare metal, single order in flight, warmed up. Actual number measured and published without rounding or cherry-picking. | Should | Benchmark report publishes the real number with full methodology disclosure, regardless of whether it meets the target. |

### 7.2 Correctness & Reliability

| ID | Requirement | Priority | Acceptance Criteria |
|---|---|---|---|
| NFR-010 | Lock-free components (SPSC ring buffers) pass ThreadSanitizer with zero reported data races under stress. | Must | Run 1M messages through each ring buffer under ThreadSanitizer → zero race reports. |
| NFR-011 | Order state machine has unit-test coverage for every `(state, event)` pair. | Must | `OrderState::Count × OrderEvent::Count` test cases → all pass → zero unhandled transitions. |
| NFR-012 | FIX parser fuzz-tested against malformed/truncated/adversarial input. | Should | Run fuzzer for 10M iterations → zero crashes, zero hangs, zero session state corruption. |
| NFR-013 | No order silently dropped: every inbound message results in a state transition or an explicit, logged rejection. | Must | Submit 10,000 orders (mix of valid, invalid, duplicate) → count(state transitions) + count(rejections) == 10,000. |

### 7.3 Observability

| ID | Requirement | Priority | Acceptance Criteria |
|---|---|---|---|
| NFR-020 | Every FIX message, risk decision, and state transition logged with monotonic timestamp. | Must | Given any `order_id`, reconstruct full lifecycle from logs alone → timeline matches actual execution. |
| NFR-021 | Latency histograms inspectable without halting the system (periodic dump). | Should | While system is running, dump current histogram state → shows real-time p50/p99/p99.9/p99.99 values. |
| NFR-022 | Hot-path logging uses a lock-free queue with separate consumer thread — no synchronous I/O inline. | Should | Capture writer thread is the only thread performing disk I/O → verified by `strace` on the hot-path threads showing no `write`/`fsync` syscalls during order processing. |

### 7.4 Testability & Reproducibility

| ID | Requirement | Priority | Acceptance Criteria |
|---|---|---|---|
| NFR-030 | Captured sessions replay deterministically on the same machine. | Must | Replay same capture file twice → byte-identical resulting state both times. |
| NFR-031 | Build and test suite runs with $0-cost tooling only. | Must | Full build + test suite runs on GitHub Actions free tier within time limits. |
| NFR-032 | OMS Core testable without live FIX session or live market data — dependency-injected interfaces. | Must | OMS Core unit tests run in <1 second with no network access required → use `InMemoryCaptureSink`, mock market data, etc. |

### 7.5 Portability & Environment

| ID | Requirement | Priority | Acceptance Criteria |
|---|---|---|---|
| NFR-040 | Builds and runs on both WSL2 and native Ubuntu. No environment-specific code paths. | Must | `cmake --build . && ctest` succeeds on both WSL2 (Ubuntu) and native Ubuntu (bare metal). |
| NFR-041 | All dependencies are free/open-source and locally buildable. | Must | `git clone` + `cmake` + `make` from a clean machine with no paid licenses → successful build. |

### 7.6 Maintainability

| ID | Requirement | Priority | Acceptance Criteria |
|---|---|---|---|
| NFR-050 | RAII throughout. No manual `new`/`delete` on the hot path outside documented pool allocators. | Should | Code review / `grep` for raw `new`/`delete` → only in `OrderPool` (documented pool allocator). |
| NFR-051 | Each architectural boundary (Gateway ↔ OMS Core ↔ Venue) defined by an explicit interface — independently replaceable and testable. | Must | Each component has a unit test suite that runs without the other components. |

---

## 8. Technical Stack

| Component | Choice | Rationale |
|---|---|---|
| **Language** | C++20 | Maximum control, manual memory management, industry standard for low-latency OMS. C++20 gives concepts, constexpr improvements for the transition table, designated initializers. |
| **Build system** | CMake | Industry standard for C++, excellent dependency management, familiar to reviewers. |
| **FIX session layer** | QuickFIX/C++ | Free, open-source. Handles session plumbing (sequence numbers, resend, logon/heartbeat) so effort goes to OMS core logic. |
| **Concurrency** | LMAX Disruptor-style SPSC ring buffers | Lock-free, cache-line-padded, `acquire`/`release` ordering (not `seq_cst`). Power-of-2 capacity with bitmask indexing. |
| **Market data** | Binance public WebSocket (BTC/USDT, ETH/USDT) | Free, no API key needed for public streams, highest volume, most reliable. |
| **Testing** | Google Test + Google Benchmark | Industry standard, CMake-native, $0, well-known to reviewers. |
| **Sanitizers** | ThreadSanitizer, AddressSanitizer | Built-in with GCC/Clang, mandatory for lock-free correctness verification. |
| **OS** | Linux (WSL2 for dev, native Ubuntu for benchmarking) | Target runtime environment. No Windows-specific code. |

---

## 9. Core Entities

> Detailed field-level specifications in [CORE_ENTITIES.md](file:///d:/kestrel/docs/CORE_ENTITIES.md). Summary below.

| Entity | Hot Path? | Domain Form | Wire Form | Key Invariant |
|---|---|---|---|---|
| **Order** | Yes | Rich object in `OrderPool` (owns exec report history) | `OrderEventWire` — fixed-size POD, 20-char ClOrdID, 12-char symbol | `cum_qty + leaves_qty == quantity` after every fill |
| **ExecutionReport** | Yes | Immutable, append-only audit trail | `ExecReportEventWire` — fixed-size POD | One per state transition, not just fills |
| **Instrument** | No (ref data) | Loaded at startup, `symbol → instrument_index` map | N/A | Static during session |
| **Position** | Yes (updated on fill) | `std::vector<Position>` indexed by `instrument_index` | N/A (stays in OMS Core thread) | Derived fold over fill-type ExecutionReports |
| **FixSession** | No | Domain object tracking session state | N/A | `seq_in`/`seq_out` persisted for crash recovery |
| **Venue** | No (config) | Configuration entity with fill model | N/A | Modeled as entity, not singleton (FR-033) |
| **MarketDataTick** | Yes | Normalized tick | `MarketDataEventWire` — fixed-size POD | Source + receipt timestamps both preserved |
| **RiskLimit** | No (config) | Data entity, not scattered constants | N/A | Every rejection references a `limit_id` |
| **CaptureRecord** | Yes (written) | Binary log entry | `CaptureRecordWire` — flat binary format | Covers all FIX messages including session-level |

---

## 10. API Boundaries

> Detailed interface specifications in [API_DESIGN.md](file:///d:/kestrel/docs/API_DESIGN.md). Summary below.

| Boundary | Kind | Mechanism | Memory Model |
|---|---|---|---|
| Pipeline stage → stage | **Hot path** | `SpscRingBuffer<WireStruct, N>` | Lock-free, `acquire`/`release`, cache-line padded |
| OMS Core → risk rules | **Hot path (debatable)** | Virtual `IRiskRule` for v1; revisit if profiling demands compile-time dispatch | Sequential, deterministic evaluation order |
| Venue simulator / real broker | **Hot path** | FIX session itself (no C++ interface needed) | TCP loopback |
| Market data source | **Cold path** | `IMarketDataFeed` (virtual dispatch) | Callback → `try_push` onto ring buffer |
| Capture / replay backend | **Cold path** | `ICaptureSink` / `IReplaySource` (virtual dispatch) | Swappable: file-backed for prod, in-memory for tests |
| Gateway ↔ QuickFIX | **External contract** | `FIX::Application` callbacks | QuickFIX owns its network thread; gateway must not block |

---

## 11. Delivery Phases

### Phase 1: Core Pipeline + State Machine + Basic FIX
**Goal:** End-to-end order round-trip through the pipeline with a stub venue.

| Deliverable | Requirements Covered |
|---|---|
| `SpscRingBuffer` with ThreadSanitizer validation | NFR-010 |
| Wire event structs (`OrderEventWire`, `ExecReportEventWire`, etc.) | API_DESIGN §2.1 |
| Table-driven order state machine with exhaustive unit tests | FR-011, FR-012, NFR-011 |
| `OrderPool` — pre-allocated, zero-heap-allocation order storage | NFR-003 |
| Inbound Gateway (`FIX::Application` → wire struct → ring buffer) | FR-001, FR-003, FR-010 |
| Outbound Gateway (ring buffer → FIX message → venue) | FR-001 |
| OMS Core thread (consume orders → state machine → produce exec reports) | FR-010, FR-013, FR-014, FR-016 |
| Latency histogram infrastructure (per-thread HDR-histogram-style) | NFR-001 |
| CMake build system with gtest integration | NFR-031, NFR-040 |
| Basic FIX 4.4 session lifecycle (Logon/Logout/Heartbeat) | FR-001 |

**Exit criteria:** Submit a NewOrderSingle via FIX → order traverses Inbound GW → OMS Core → Outbound GW → stub venue acks → Execution Report traverses back → client receives ack. All under ThreadSanitizer with zero races.

---

### Phase 2: Venue Simulator + Market Data
**Goal:** Replace stub venue with a realistic, market-data-driven simulated venue.

| Deliverable | Requirements Covered |
|---|---|
| Binance WebSocket market data ingester (BTC/USDT, ETH/USDT) | FR-040, FR-041 |
| `MarketDataEventWire` normalization pipeline | FR-031 |
| Venue Core with immediate-full-fill model | FR-030 |
| Venue FIX acceptor (Process B) | FR-030, FR-033 |
| Two-process startup/shutdown orchestration | HLA §4 |
| Reconnect handling for WebSocket disconnects | FR-041 |
| Cancel/Replace request handling (35=F, 35=G) | FR-013 |
| Duplicate ClOrdID detection | FR-014 |

**Exit criteria:** Full end-to-end order lifecycle with venue filling at live Binance prices. Kill and restart WebSocket → venue pauses → reconnects → resumes without stale price fills.

---

### Phase 3: Capture/Replay + Risk Checks
**Goal:** Complete the correctness and observability story.

| Deliverable | Requirements Covered |
|---|---|
| Binary capture log (flat format, capture writer thread) | FR-050, NFR-022 |
| Deterministic replay from capture log | FR-051, NFR-030 |
| Replay-as-benchmark-harness mode | FR-052 |
| Risk rules: max notional (FR-020), max quantity (FR-021), fat-finger (FR-022) | FR-020–FR-024 |
| Rate limiter (FR-023) | FR-023 |
| In-memory position ledger (array-indexed) | FR-015 |
| Sequence gap handling (ResendRequest/SequenceReset) | FR-002 |
| Session state persistence across restarts | FR-005 |

**Exit criteria:** Capture a session with 10,000 orders → replay → byte-identical resulting order/position state. All risk rules fire with logged `rule_id` attribution. Sequence gap injection test passes.

---

### Phase 4: Benchmarking + Hardening
**Goal:** Measure, publish, and harden.

| Deliverable | Requirements Covered |
|---|---|
| Bare-metal Ubuntu benchmarking environment setup | NFR-002, NFR-004 |
| Full latency benchmark suite (p50/p99/p99.9/p99.99) | NFR-001, NFR-004 |
| Zero-heap-allocation verification on hot path | NFR-003 |
| FIX parser fuzz testing | NFR-012 |
| Adversarial venue modes (partial fills, delayed acks, session drops) | FR-032 |
| Latency histogram live-dump (inspectable without halting) | NFR-021 |
| CI pipeline on GitHub Actions free tier | NFR-031 |
| Published benchmark report with full methodology disclosure | NFR-002, NFR-004 |

**Exit criteria:** Published benchmark report with honest p99.9 latency number on bare metal. ThreadSanitizer clean under sustained load. Fuzz testing: zero crashes over 10M iterations.

---

## 12. Success Metrics / KPIs

### Correctness KPIs

| Metric | Target | Measurement Method |
|---|---|---|
| State machine coverage | 100% of `(OrderState × OrderEvent)` pairs tested | Unit test that programmatically enumerates all pairs |
| ThreadSanitizer races | 0 data races under 1M-message stress | TSan CI run |
| Silent order drops | 0 orders unaccounted for | `count(transitions) + count(rejections) == count(submissions)` |
| Deterministic replay | Byte-identical state on replay | Automated comparison in CI |
| Fuzz testing crashes | 0 crashes, 0 hangs in 10M iterations | Fuzzer harness in CI |

### Performance KPIs

| Metric | Target | Measurement Method |
|---|---|---|
| Hot-path heap allocations | 0 | Custom allocator trap on pinned threads |
| E2E order-to-ack p99.9 (bare metal) | < 1ms (aspirational, actual number published honestly) | Google Benchmark + HDR histogram |
| Ring buffer stage-to-stage latency | Measured and reported as p50/p99/p99.9/p99.99 | Per-thread histogram, periodic dump |
| WSL2 vs bare-metal labeling | 100% of latency figures correctly labeled | Report review |

### Engineering Rigor KPIs

| Metric | Target | Measurement Method |
|---|---|---|
| Raw `new`/`delete` on hot path | 0 outside `OrderPool` | `grep` + code review |
| Risk rejection attribution | 100% of rejections have `rule_id` | Unit tests + log audit |
| Build on clean machine | `git clone` → `cmake` → `make` → `ctest` succeeds | CI on GitHub Actions free tier |
| Documentation honesty | All provisional numbers labeled as such, all limitations disclosed | Self-audit |

---

## 13. Assumptions & Dependencies

| Assumption | Risk if violated | Mitigation |
|---|---|---|
| Binance public WebSocket remains free for non-commercial use | Market data feed breaks | `IMarketDataFeed` interface allows swap to Coinbase or other free feed |
| QuickFIX/C++ is adequate for the session layer | Session-layer bug blocks a Must requirement | Accepted as known dependency risk; hand-rolled FIX engine is a documented escape hatch |
| Bare-metal Ubuntu available for Phase 4 | Cannot produce representative latency numbers | All pre-Phase-4 numbers labeled "WSL2-directional" per NFR-002 |
| GCC 12+ available on target Ubuntu | C++20 features unavailable | Standard Ubuntu 22.04+ ships GCC 12 |
| Solo developer bandwidth | Phases take longer than estimated | Phases are independent milestones — each is demo-able on its own |

---

## 14. Explicitly Out of Scope

| Item | Reason |
|---|---|
| Matching engine / limit order book with price-time priority | Simulated venue plays this role |
| Multi-asset-class support beyond FIX/OMS exercise needs | Scope control |
| Paid market data or FIX drop-copy feeds | $0 budget constraint |
| Authentication/entitlements beyond FIX session-level | Not the learning objective |
| GUI / dashboard / web UI | Not the learning objective; CLI + log analysis is sufficient |
| Multi-venue smart order routing | Single venue in v1; swappability via FR-033 leaves the door open |
| Regulatory compliance (MiFID, Reg NMS, etc.) | Portfolio project, not production trading system |

---

## 15. Risk Register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| QuickFIX/C++ performance overhead dominates latency budget | Medium | High | Documented escape hatch: hand-rolled FIX parser. Measure first, replace only if needed. |
| Lock-free ring buffer correctness bug under edge cases | Medium | Critical | ThreadSanitizer mandatory in CI. Stress tests with 1M+ messages. |
| Binance WebSocket API changes or rate-limits | Low | Medium | `IMarketDataFeed` abstraction allows swap. Multiple pairs (BTC + ETH) provide redundancy. |
| Scope creep into matching engine / order book | Medium | Medium | Explicitly out-of-scope in PRD and Charter. Venue is a separate process — physically hard to accidentally merge. |
| WSL2 jitter makes development-time latency numbers misleading | High | Low | NFR-002 mandates labeling. All optimization deferred to Phase 4 on bare metal. |

---

## 16. Glossary

| Term | Definition |
|---|---|
| **FIX** | Financial Information eXchange — industry-standard protocol for electronic trading |
| **OMS** | Order Management System — software that tracks orders through their lifecycle |
| **SPSC** | Single Producer, Single Consumer — lock-free queue pattern |
| **LMAX Disruptor** | High-performance inter-thread messaging pattern using ring buffers and pinned threads |
| **Wire struct** | Fixed-size, flat, POD struct safe to pass through ring buffers by value — no pointers, no heap |
| **Domain form** | Rich C++ object with full semantics (strings, vectors, virtual dispatch) — used off the hot path |
| **Hot path** | The latency-critical order-receipt → risk-check → routing path — zero heap allocation, no mutexes |
| **Cold path** | Startup, configuration, and non-latency-critical paths — normal C++ idioms allowed |
| **p99.9** | 99.9th percentile latency — the latency that 99.9% of events are faster than |
| **HDR Histogram** | High Dynamic Range histogram — log-linear bucketing for latency recording with minimal overhead |
| **ClOrdID** | Client Order ID — client-assigned identifier for an order, unique per FIX session |
| **ExecType** | Execution Report type — indicates the reason for the report (New, Fill, Cancel, Reject, etc.) |
| **QuickFIX/C++** | Open-source FIX engine providing session-layer plumbing |
| **ThreadSanitizer (TSan)** | Compiler instrumentation tool that detects data races in concurrent code |
| **RAII** | Resource Acquisition Is Initialization — C++ idiom for deterministic resource management |

---

## Appendix A: Document Traceability Matrix

| PRD Section | Source Document | Source Section |
|---|---|---|
| §4 Product Overview | [HIGH_LEVEL_ARCHITECTURE.md](file:///d:/kestrel/docs/HIGH_LEVEL_ARCHITECTURE.md) | §1–§4 |
| §6.1 FIX Session Mgmt | [REQUIREMENTS.md](file:///d:/kestrel/docs/REQUIREMENTS.md) | §2.1 |
| §6.2 Order Management | [REQUIREMENTS.md](file:///d:/kestrel/docs/REQUIREMENTS.md) | §2.2 |
| §6.3 Risk Checks | [REQUIREMENTS.md](file:///d:/kestrel/docs/REQUIREMENTS.md) | §2.3 |
| §6.4 Simulated Venue | [REQUIREMENTS.md](file:///d:/kestrel/docs/REQUIREMENTS.md) | §2.4 |
| §6.5 Market Data | [REQUIREMENTS.md](file:///d:/kestrel/docs/REQUIREMENTS.md) | §2.5 |
| §6.6 Capture & Replay | [REQUIREMENTS.md](file:///d:/kestrel/docs/REQUIREMENTS.md) | §2.6 |
| §7 Non-Functional Reqs | [REQUIREMENTS.md](file:///d:/kestrel/docs/REQUIREMENTS.md) | §3 |
| §8 Technical Stack | [PROJECT_CHARTER.md](file:///d:/kestrel/docs/PROJECT_CHARTER.md) | Tech Stack, §5 resolved decisions |
| §9 Core Entities | [CORE_ENTITIES.md](file:///d:/kestrel/docs/CORE_ENTITIES.md) | §2 |
| §10 API Boundaries | [API_DESIGN.md](file:///d:/kestrel/docs/API_DESIGN.md) | §1–§5 |
| §11 Delivery Phases | [PROJECT_CHARTER.md](file:///d:/kestrel/docs/PROJECT_CHARTER.md) | Pace, §5 resolved decisions |
| Low-level implementation | [LOW_LEVEL_DESIGN.md](file:///d:/kestrel/docs/LOW_LEVEL_DESIGN.md) | §1–§9 |

---

*This PRD consolidates all existing design documentation into a single authoritative reference. The source documents remain canonical for implementation-level detail; this PRD is the decision record and scope contract.*
