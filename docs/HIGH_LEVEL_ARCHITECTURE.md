# High-Level Architecture — OMS Core + FIX Gateways

**Document ID:** DESIGN-003
**Status:** Draft v1
**Traces to:** PROJECT_CHARTER.md (concurrency model), 03_API_DESIGN.md (all interfaces)

---

## 1. Deployment shape: two processes, not one

The single biggest architectural decision at this level is that the system is
**two separate OS processes on the same machine**, talking over a real FIX
session on loopback TCP — not one process with an in-process "venue" object.

| | One process, in-process venue | Two processes, loopback FIX |
|---|---|---|
| Realism | Venue behaves like a function call | Venue behaves like a real counterparty — actual TCP, actual FIX serialization/deserialization, actual kernel-mediated latency |
| Gateway resilience testing | Can't test reconnect/sequence-gap logic (FR-002) without faking a disconnect | Kill process B, the Outbound Gateway genuinely has to handle a dropped session and resync sequence numbers |
| Latency measurement | Measures only OMS Core logic | Measures the full realistic path, including the network hop — the number you'd actually want to defend |
| Complexity | Lower | Higher — two processes to start, two configs, two things that can crash |

Given the project's explicit goal (p99.9 rigor, not just working code), the
two-process shape is worth the added complexity: it's the only way FR-002
(sequence gap handling) and FR-032 (venue simulating disconnects) get
exercised honestly, and it's also a direct, concrete link back to the
Charter's WSL2-vs-bare-metal decision — loopback TCP latency is exactly the
kind of number that will look measurably different (and worse) under
virtualization, giving you a real before/after data point once the
bare-metal environment is set up.

- **Process A — "OMS system":** Inbound Gateway, OMS Core, Outbound Gateway.
- **Process B — "Venue simulator":** Market Data Ingester, Venue Core (fill
  model + its own FIX acceptor).

---

## 2. Process A — OMS system

Three pinned threads, connected by the `SpscRingBuffer`s from 03_API_DESIGN.md.

1. **Inbound Gateway thread** — owns the FIX session to the client. On
   `fromApp`, translates and `try_push`es `OrderEventWire`/`CancelEventWire`
   onto the buffer feeding OMS Core. Also drains a buffer of outbound
   `ExecReportEventWire`s (produced by OMS Core) and sends them to the client
   as FIX Execution Reports.
2. **OMS Core thread** — the single-threaded heart of the system: order
   state machine (FR-011), risk checks (FR-020 to FR-024), position ledger
   (FR-015). Consumes from the Inbound Gateway's buffer and from the
   Outbound Gateway's exec-report buffer; produces onto the Inbound Gateway's
   send buffer and the Outbound Gateway's routing buffer. Kept single-
   threaded deliberately — splitting risk checks onto their own thread is a
   possible later optimization, not a v1 decision (same "measure before
   you split it" logic as the risk-rule dispatch question in 03_API_DESIGN.md).
3. **Outbound Gateway thread** — owns the FIX session to the venue. Drains
   the OMS Core's routing buffer, sends `NewOrderSingle`/`OrderCancelRequest`
   FIX messages to the venue; on `fromApp`, translates incoming Execution
   Reports into `ExecReportEventWire` and pushes them onto OMS Core's buffer.

**Capture (FR-050, NFR-022):** both gateways push a copy of every raw FIX
message onto a dedicated capture ring buffer, drained by a **fourth thread**
whose only job is writing to the append-only capture log. This keeps disk
I/O off every thread that's actually on the latency-critical path — logging
never blocks routing.

---

## 3. Process B — Venue simulator

Two threads:

1. **Market Data Ingester thread** — owns the WebSocket connection to a free
   public crypto feed (FR-040), normalizes ticks into `MarketDataEventWire`,
   pushes onto a buffer feeding the Venue Core. Handles reconnects (FR-041)
   without letting a stale price leak into the fill model.
2. **Venue Core / FIX acceptor thread** — owns the FIX session to Process A.
   Consumes market data ticks to maintain a reference price per symbol,
   consumes inbound orders from Process A, applies the configured fill model
   (immediate / probabilistic-partial / latency-delayed — still an open
   question from 01_REQUIREMENTS.md §5), and emits Execution Reports back.

---

## 4. Startup / shutdown ordering

Getting this sequence wrong is a common source of "works when I start it
just right, breaks otherwise" bugs — worth pinning down now rather than
discovering it experimentally later.

**Startup:**
1. Process B starts first: Venue Core constructs its ring buffers, then the
   Market Data Ingester connects and starts publishing ticks. Venue Core
   waits for at least one tick before opening its FIX acceptor (no
   quoting/filling against an undefined reference price).
2. Process A starts: OMS Core constructs its ring buffers and capture
   writer, then both Gateways start. Outbound Gateway's FIX initiator
   connects to Process B's now-open acceptor. Inbound Gateway's acceptor
   opens last, once the path all the way through to the venue is confirmed
   live — no accepting client orders into a system that can't route them
   anywhere yet.

**Shutdown (reverse, and drain-before-close):**
1. Inbound Gateway stops accepting new orders, but stays up until its
   inbound ring buffer is drained (in-flight orders finish their trip
   through OMS Core).
2. Outbound Gateway similarly drains its routing buffer before closing the
   FIX session to the venue.
3. Capture writer thread drains its buffer to disk last, so the capture log
   is complete before the process exits — a partial capture log silently
   breaks FR-051's determinism guarantee on replay.

---

## 5. Buffer sizing — a placeholder, not a decision

Every `SpscRingBuffer` capacity in this architecture is a compile-time
constant that should start as a round, generous guess (e.g. 4096 entries)
and be revisited once you have real burst-rate data from the benchmarking
phase. Documenting a made-up "optimal" number here would be exactly the
kind of unearned precision this project is trying to avoid — the honest
answer right now is "big enough not to be the first bottleneck, to be
tuned against evidence later."

---

## 6. What this architecture deliberately defers

- Exact fill model for the venue (open question, 01_REQUIREMENTS.md §5)
- Whether risk-rule dispatch needs to move off virtual calls (03_API_DESIGN.md §3.1)
- Whether OMS Core stays single-threaded permanently or gets split further
- Ring buffer capacities (§5 above)

Each of these is a "decide once you can measure it" item, not a gap in the
design — a design that pretends to know these numbers before a single
benchmark has run would be less honest than one that names them explicitly
as open.

**Next suggested task:** the FIX message/field mapping — the concrete
dictionary of which FIX tags populate which wire struct fields, for both the
client-facing and venue-facing sessions.
