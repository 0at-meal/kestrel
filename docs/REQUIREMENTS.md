# Requirements Specification — OMS Core + FIX Gateways

**Document ID:** REQ-001
**Status:** Draft v1
**Traces to:** PROJECT_CHARTER.md
**Convention:** Each requirement has an ID, a MoSCoW priority (Must/Should/Could/Won't),
and — for the ones that actually drive design decisions — acceptance criteria. IDs are
permanent once assigned; a dropped requirement is marked `[DEPRECATED]`, never reused,
never silently deleted. This is how you keep a requirements doc trustworthy over a
multi-month solo project when your own memory of "why did I build it this way" fades.

---

## 1. System Context

Four components, three trust/protocol boundaries:

```
[Client/Order Source] --FIX--> [Inbound Gateway] --internal--> [OMS Core]
                                                                     |
                                                                internal
                                                                     |
                                                                     v
[Simulated Venue] <--FIX-- [Outbound Gateway] <-------------- (routing)
        ^
        |
   seeded by real crypto market data (WebSocket -> internal price feed)
```

The OMS Core never speaks FIX directly — it only sees a normalized internal order/event
representation. Gateways are translation boundaries. This is the same separation real
sell-side OMS use, and it's what lets you unit-test OMS logic without a FIX session in
the loop at all.

---

## 2. Functional Requirements

### 2.1 FIX Session Management

| ID | Requirement | Priority |
|---|---|---|
| FR-001 | System shall establish and maintain a FIX session (Logon/Logout/Heartbeat/TestRequest) on both inbound and outbound gateways, using QuickFIX/C++ for session-layer mechanics. | Must |
| FR-002 | System shall detect sequence number gaps and issue/respond to ResendRequest and SequenceReset (gap-fill) per FIX session protocol. | Must |
| FR-003 | System shall reject and log any message failing FIX tag/value syntax validation, without crashing or corrupting session state. | Must |
| FR-004 | System shall support configurable FIX dictionary version (start with FIX 4.4; architecture must not hardcode assumptions that block a later 5.0 SP2 upgrade). | Should |
| FR-005 | System shall persist session state (sequence numbers) across restarts so a crash doesn't desynchronize the session. | Should |

**Acceptance criteria for FR-002** (this is the one that separates a toy from a real
gateway): inject an artificial sequence gap in a test harness; system must issue a
ResendRequest, correctly process the gap-fill response, and resume normal message
processing with no order state corruption — verified by comparing pre/post order book
snapshots.

### 2.2 Order Management (OMS Core)

| ID | Requirement | Priority |
|---|---|---|
| FR-010 | System shall accept New Order Single (35=D) messages and create an order in state `PendingNew`. | Must |
| FR-011 | System shall implement the full order state machine: `PendingNew -> New -> {PartiallyFilled, Filled, PendingCancel, PendingReplace, Rejected, Expired, DoneForDay}` with `Cancelled`/`Replaced` as terminal states reachable from the Pending* states. | Must |
| FR-012 | System shall reject any state transition not explicitly defined in the state machine (e.g. Cancel request on an already-Filled order) and generate an appropriate Reject/CancelReject response rather than silently ignoring it. | Must |
| FR-013 | System shall support Order Cancel Request (35=F) and Order Cancel/Replace Request (35=G), correctly matching them to the original order via ClOrdID/OrigClOrdID chain. | Must |
| FR-014 | System shall reject duplicate ClOrdID values within a session (idempotency / duplicate-submission protection). | Must |
| FR-015 | System shall maintain a position ledger, updated on every fill, queryable at any point in time. | Should |
| FR-016 | System shall generate Execution Reports (35=8) reflecting every state transition, not just fills. | Must |

**Acceptance criteria for FR-011/FR-012**: the state machine must be implemented as an
explicit, exhaustively-tested table (not scattered if/else chains) — every (state,
event) pair either has a defined transition or an explicit reject; there is no
undefined/"whatever happens to happen" behavior. This is testable directly: a unit
test can enumerate all (state, event) pairs and assert every one is handled.

### 2.3 Risk Checks (Pre-Trade)

| ID | Requirement | Priority |
|---|---|---|
| FR-020 | System shall reject orders exceeding a configurable max notional value per order. | Must |
| FR-021 | System shall reject orders exceeding a configurable max quantity per order. | Must |
| FR-022 | System shall apply a fat-finger check (price outside N% of last traded price) before routing to the venue. | Should |
| FR-023 | System shall rate-limit inbound order messages per session to a configurable max orders/sec, rejecting excess with a clear reason code rather than silently dropping. | Should |
| FR-024 | Risk checks shall execute on the hot path before an order is routed outbound, and every rejection shall be attributable to a specific, logged rule. | Must |

### 2.4 Simulated Venue

| ID | Requirement | Priority |
|---|---|---|
| FR-030 | Venue simulator shall accept inbound FIX orders and respond with Execution Reports (New ack, Fill/Partial Fill, Reject) according to a configurable fill model. | Must |
| FR-031 | Venue simulator's reference price shall be driven by a real, free, live crypto market data feed (public WebSocket), translated into an internal price series. | Must |
| FR-032 | Venue simulator shall support configurable behaviors for adversarial/edge-case testing: partial fills, delayed acks, rejects, out-of-order execution reports, session drops mid-order. | Should |
| FR-033 | Venue simulator shall be swappable behind an interface, so the OMS/Gateway can later be pointed at a different counterparty without code changes. | Could |

### 2.5 Market Data Ingestion

| ID | Requirement | Priority |
|---|---|---|
| FR-040 | System shall connect to a free public crypto WebSocket feed (e.g. Binance) and normalize incoming ticks into an internal price/quote representation. | Must |
| FR-041 | System shall handle feed disconnects/reconnects gracefully without propagating a stale price into the venue simulator. | Must |
| FR-042 | System shall optionally translate normalized ticks into genuine FIX MarketData messages (35=W/X) for realism/testing of MD-handling code paths. | Could |

### 2.6 Capture & Replay

| ID | Requirement | Priority |
|---|---|---|
| FR-050 | System shall capture every inbound and outbound FIX message with a monotonic high-resolution timestamp, to a durable log. | Must |
| FR-051 | System shall be able to replay a captured session deterministically, producing byte-identical resulting order/position state as the original run. | Must |
| FR-052 | Replay shall be usable as a benchmark harness input (fixed, repeatable workload for latency measurement). | Should |

### 2.7 Explicitly Out of Scope

| ID | Item | Reason |
|---|---|---|
| FR-X01 | Matching engine / limit order book with price-time priority | Simulated venue plays this role — see Charter |
| FR-X02 | Multi-asset-class support beyond what's needed to exercise FIX/OMS logic | Scope control |
| FR-X03 | Paid market data or FIX drop-copy feeds | Budget constraint |
| FR-X04 | Authentication/entitlements beyond FIX session-level | Not the learning objective |

---

## 3. Non-Functional Requirements

### 3.1 Performance & Latency

| ID | Requirement | Priority |
|---|---|---|
| NFR-001 | Internal stage-to-stage handoff latency (ring buffer publish-to-consume) shall be measured and reported as p50/p99/p99.9/p99.99, not just an average. | Must |
| NFR-002 | Latency measurements taken on WSL2 shall be labeled "directional" in all reports/dashboards; only measurements taken on native bare-metal Linux shall be labeled as representative figures. | Must |
| NFR-003 | Hot path (order receipt -> risk check -> routing decision) shall perform zero heap allocation, verified by instrumentation or an allocator that traps/logs unexpected allocations. | Should |
| NFR-004 | End-to-end order-to-ack latency target (bare metal, warmed-up, single order in flight): p99.9 under 1ms is the aspirational target; the actual achieved number is to be measured, published, and never silently rounded or cherry-picked. | Should |

*Target numbers in NFR-004 are provisional and exist to give the optimization phase
something concrete to chase — they will be revisited once you have a first real
measurement. A rigor-minded doc says so explicitly instead of pretending the number
was always right.*

### 3.2 Correctness & Reliability

| ID | Requirement | Priority |
|---|---|---|
| NFR-010 | Concurrent/lock-free components (ring buffers, SPSC/MPSC queues) shall pass ThreadSanitizer with zero reported data races under a representative stress workload. | Must |
| NFR-011 | The order state machine shall have unit-test coverage for every defined and every explicitly-rejected (state, event) transition (see FR-011/012). | Must |
| NFR-012 | FIX parser shall be fuzz-tested against malformed/truncated/adversarial input without crashing, hanging, or corrupting session state. | Should |
| NFR-013 | No order shall be silently dropped: every inbound message results in either a state transition or an explicit, logged rejection. | Must |

### 3.3 Observability

| ID | Requirement | Priority |
|---|---|---|
| NFR-020 | Every FIX message, risk decision, and state transition shall be logged with a monotonic timestamp sufficient to reconstruct the full lifecycle of any order after the fact. | Must |
| NFR-021 | Latency histograms shall be exposed in a form that can be inspected without halting the running system (e.g. periodic dump, not just end-of-run). | Should |
| NFR-022 | Logging on the hot path shall not itself become a latency source — use a lock-free logging queue with a separate consumer thread, not synchronous I/O inline. | Should |

### 3.4 Testability & Reproducibility

| ID | Requirement | Priority |
|---|---|---|
| NFR-030 | Any captured session (FR-051) shall be replayable to produce identical results on a different run, on the same machine. | Must |
| NFR-031 | Build and test suite shall run with $0-cost tooling only (e.g. GitHub Actions free tier, local CI), no paid CI/CD. | Must |
| NFR-032 | Core OMS logic shall be testable without a live FIX session or live market data (dependency-injected interfaces, not hard-wired sockets). | Must |

### 3.5 Portability & Environment

| ID | Requirement | Priority |
|---|---|---|
| NFR-040 | Codebase shall build and run correctly on both WSL2 and native Ubuntu without environment-specific code paths (no hardcoded `/mnt/c` paths, no Windows-specific assumptions). | Must |
| NFR-041 | All dependencies shall be free/open-source and locally buildable — no dependency shall require a paid license or paid API key to build or run the core system. | Must |

### 3.6 Maintainability

| ID | Requirement | Priority |
|---|---|---|
| NFR-050 | Code shall follow RAII / no manual `new`/`delete` on the hot path outside of controlled, documented pool allocators. | Should |
| NFR-051 | Each architectural boundary (Gateway <-> OMS Core <-> Venue) shall be defined by an explicit interface, so any one component is independently replaceable/testable. | Must |

---

## 4. Assumptions & Dependencies

- Free crypto WebSocket feeds (Binance/Coinbase public API) remain available and usable
  for personal, non-commercial, non-high-frequency polling under their public terms of use.
- QuickFIX/C++ is adopted as-is for the session layer; any bugs/limitations in it are
  accepted as a known dependency risk rather than a reason to fork/patch it, unless a
  specific defect blocks a Must requirement.
- Bare-metal benchmark numbers (NFR-002/004) are not available until the dual-boot
  Ubuntu environment from the Charter is set up; until then, all published latency
  figures are WSL2-sourced and marked directional per NFR-002.

## 5. Open Questions (to resolve before or during design phase)

- Exact FIX dictionary/version pin (4.4 vs 5.0 SP2) — affects available message types.
- Whether position/ledger persistence (FR-015) needs to survive process restart, or is
  in-memory-only for v1.
- Precise fill model for the venue simulator (immediate full fill vs probabilistic
  partial fills vs latency-delayed fills) — affects how "realistic" early testing feels.

---

**Next suggested task:** high-level architecture & component design (translating these
requirements into a concrete module boundary, threading/pipeline layout, and data flow
diagram).
