# Project Charter — Solo Indie OMS for Global Markets

## Mission
Build an institutional-grade Order Management System core, connected to inbound/outbound
FIX gateways, as a solo indie dev learning project. Goal: demonstrate p99.9-grade
engineering rigor — in correctness, protocol conformance, and measured latency —
on a $0 budget, no paid infra.

## Constraints
- Budget: $0. No paid cloud, no paid data feeds, no paid tooling.
- Solo developer, bedroom setup.
- Primary dev environment: WSL2 (Ubuntu on Windows) — available today, zero setup cost.
- Latency/benchmark measurement environment: native dual-boot Ubuntu (bare metal),
  set up later specifically for the benchmarking phase. WSL2 numbers are directional
  only (virtualization jitter, no reliable PMU/perf-counter access, no true core
  isolation) and will be explicitly documented as such wherever used.

## Scope
**In scope:**
- OMS Core: order state machine, risk checks, order routing logic
- Inbound FIX Gateway (client/broker → OMS)
- Outbound FIX Gateway (OMS → venue)
- A simulated FIX-speaking venue (counterparty) to test against — this is the
  equivalent of a bank's in-house FIX UAT/cert simulator
- Venue price/order-flow generation seeded by real, free live crypto market data
  (e.g. Binance/Coinbase public WebSocket feeds), translated into genuine FIX
  MarketData messages — so every wire in the system speaks real FIX, while price
  action reflects real market dynamics rather than pure synthetic noise
- Session capture/replay for deterministic regression and benchmark testing

**Out of scope (explicitly):**
- Building a matching engine / limit order book (an external simulated venue plays this role)
- Real paid market data or FIX drop-copy feeds
- Multi-asset-class complexity beyond what's needed to exercise the OMS/FIX logic

## Tech Stack
- Language: C++ (max control, manual memory management, industry standard for this domain)
- FIX session layer: QuickFIX/C++ (free, open-source) — effort goes into OMS core logic,
  not reinventing session-layer plumbing (sequence numbers, resend logic, logon/heartbeat)
- Concurrency model: LMAX Disruptor-style pipeline — pinned threads per stage
  (network I/O → FIX session → OMS core/risk → outbound), connected by lock-free
  SPSC/MPSC ring buffers. No mutexes on the hot path.
- OS target: Linux (WSL2 for dev, native Ubuntu for latency measurement)

## Engineering Rigor Definition
"p99.9 rigor" = correctness/protocol conformance AND latency discipline, both from day 1,
not sequential phases. Concretely:
- Latency histograms + percentile tracking (p50/p99/p99.9/p99.99) are a first-class
  deliverable, built early, not bolted on at the end
- Protocol conformance (FIX message correctness, state machine correctness under
  malformed/adversarial input, sequence gap/resend handling) is tested rigorously
  alongside performance
- Measurement environment caveats are always disclosed (WSL2 vs bare metal)

## Pace
Intensive, deep-dive pace. Target: ~80% functional completion (core OMS + FIX gateways +
simulated venue + basic pipeline working end-to-end) within weeks. Remaining ~20%
(latency optimization, lock-free correctness hardening, bare-metal benchmarking,
chaos/edge-case testing) worked over the following weeks-to-months.

## Status
Charter locked. Ready for first task assignment.
