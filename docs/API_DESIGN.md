# API Design — Internal Component Interfaces

**Document ID:** DESIGN-002
**Status:** Draft v1
**Traces to:** 01_REQUIREMENTS.md (NFR-051), 02_CORE_ENTITIES.md (§1, domain vs wire forms)

---

## 1. Governing principle: two kinds of API, deliberately different

This is the single most important decision in this document, and it has to be
stated before any interface signature, or the interfaces below will look
inconsistent instead of intentional.

- **Cold-path APIs** — used for startup-time component selection and anything
  off the hot path (choosing a market data source, choosing a capture sink,
  configuring risk limits). These are allowed to use normal C++ virtual
  dispatch (abstract base classes). The cost of a vtable indirection is
  irrelevant here.
- **Hot-path API** — the order-received-to-order-routed path. This API is
  **not** a set of virtual interfaces. It is a concrete, statically-typed,
  fixed-size ring buffer contract moving POD "wire" structs between pinned
  threads (per the Charter's LMAX-style pipeline). No vtables, no heap
  allocation, no exceptions on this path (NFR-003, NFR-010).

Putting a virtual `IRiskCheck::evaluate()` call on the hot path because "it's
just an interface" is exactly the kind of decision that quietly caps your
p99.9 number and is invisible in a code review unless someone is looking for
it. This document keeps the two worlds explicitly separate so that mistake is
structurally harder to make.

---

## 2. Hot-path wire API

### 2.1 Wire event structs
Fixed-size, no pointers, no `std::string`, trivially copyable — safe to move
through a ring buffer by value.

```cpp
struct OrderEventWire {
    char        cl_ord_id[20];
    char        symbol[12];
    Side        side;
    OrderType   order_type;
    double      price;
    double      quantity;
    uint32_t    session_id;
    uint64_t    mono_ts_ns;
};

struct CancelEventWire {
    char        cl_ord_id[20];
    char        orig_cl_ord_id[20];
    uint32_t    session_id;
    uint64_t    mono_ts_ns;
};

struct ExecReportEventWire {
    char        exec_id[20];
    uint64_t    order_id;      // internal id, not cl_ord_id
    ExecType    exec_type;
    OrdStatus   ord_status;
    double      last_qty;
    double      last_px;
    double      cum_qty;
    double      leaves_qty;
    uint64_t    mono_ts_ns;
};

struct MarketDataEventWire {
    char        symbol[12];
    double      bid_px;
    double      ask_px;
    double      last_px;
    uint64_t    mono_ts_ns;
};
```

**Open question carried forward:** one ring buffer per event type (simpler,
no tagged union, but more buffers to drain in order) vs a single
`PipelineEvent` tagged union/variant per stage boundary (fewer buffers, but
adds a discriminant + max-size padding, and consumers must switch on type).
Recommend starting with per-type buffers — simpler to reason about and to
benchmark independently — and only merging into a variant if profiling shows
buffer-draining overhead matters. Revisit once you have real numbers.

### 2.2 Ring buffer contract
Every pipeline stage boundary is one instance of this template. Single
producer, single consumer — no locks, no CAS loops beyond what's needed for
the head/tail indices.

```cpp
template <typename T, size_t Capacity>
class SpscRingBuffer {
public:
    // Producer side. Returns false if full (backpressure signal — caller
    // decides what "full" means for their stage, e.g. drop-oldest for
    // market data, block/retry for orders). Never throws, never allocates.
    bool try_push(const T& item) noexcept;

    // Consumer side. Returns false if empty.
    bool try_pop(T& out) noexcept;

    // Approximate — for monitoring/backpressure decisions only, not for
    // correctness (a stale read is fine here, per relaxed-ordering rules
    // typical of SPSC ring buffer implementations).
    size_t size_approx() const noexcept;
};
```

**Ownership rule:** each buffer instance is owned by the consumer thread's
stage struct, constructed once at startup, and never resized or reallocated
at runtime. Capacity is a compile-time constant chosen per stage based on
expected burst size — this is a benchmarking-phase tuning input, not a
day-one guess to get precisely right.

---

## 3. Cold-path component interfaces

These use ordinary virtual dispatch — chosen once at startup (or per test),
not called per-message on the hot path.

### 3.1 Risk evaluation
Risk checks *do* run inline in the OMS Core stage before routing (FR-024),
which makes this the one interface where the cold-path/hot-path line is
genuinely debatable.

```cpp
struct RiskDecision {
    bool     passed;
    uint32_t rule_id;   // FR-024: every rejection traces to a specific rule
};

class IRiskRule {
public:
    virtual ~IRiskRule() = default;
    virtual RiskDecision evaluate(const OrderEventWire& order) const noexcept = 0;
};
```

**Explicit trade-off, not a decided answer:** a `std::vector<std::unique_ptr<IRiskRule>>`
evaluated in sequence is simple and swappable at runtime, at the cost of one
vtable call per rule per order. A compile-time rule list (variadic template
pack or `std::tuple` of concrete rule types, unrolled via fold expression)
removes the indirection entirely but makes rules fixed at compile time. Given
the "Should" priority on NFR-003 (not "Must"), start with the simple virtual
version — get correctness and the exhaustive-transition-table testing (FR-011)
solid first — and only move to the compile-time version if benchmarking shows
risk evaluation is a measurable fraction of your p99.9 budget.

### 3.2 Market data feed
```cpp
class IMarketDataFeed {
public:
    virtual ~IMarketDataFeed() = default;
    virtual void connect() = 0;
    virtual void subscribe(std::string_view symbol,
                            std::function<void(const MarketDataEventWire&)> on_tick) = 0;
    virtual void disconnect() noexcept = 0;
};
```
The callback itself must only do one thing: translate and `try_push` onto the
market-data ring buffer. No processing inside the callback — it runs on
whatever thread the feed implementation drives (its own I/O thread for a
WebSocket-based `BinanceMarketDataFeed`), and that thread is not one of your
pinned pipeline threads.

### 3.3 Capture sink / replay source
```cpp
class ICaptureSink {
public:
    virtual ~ICaptureSink() = default;
    virtual void record(const CaptureRecordWire& rec) = 0;
};

class IReplaySource {
public:
    virtual ~IReplaySource() = default;
    // Returns false when exhausted. Drives replay at the recorded mono_ts_ns
    // pacing, or as fast as possible in "benchmark mode" — mode is a ctor
    // param on the concrete implementation, not part of this interface.
    virtual bool next(CaptureRecordWire& out) = 0;
};
```
Swappable per FR-050/051: a `FileCaptureSink` for real runs, an
`InMemoryCaptureSink` for unit tests, and `FileReplaySource` /
`InMemoryReplaySource` mirroring them — this is what makes NFR-032 ("OMS core
testable without a live FIX session") actually achievable in practice, not
just an aspiration in the requirements doc.

---

## 4. Gateway boundary: QuickFIX integration contract

The Gateways are the one place an *external* interface is imposed on you:
QuickFIX/C++ requires implementing `FIX::Application`. This callback
interface is the literal glue between the FIX session and the internal wire
API — worth documenting explicitly since it's easy to accidentally do
too much work inside these callbacks.

```cpp
class InboundGatewayApplication : public FIX::Application {
public:
    void fromApp(const FIX::Message& msg, const FIX::SessionID&)
        override EXCEPT(FIX::FieldNotFound, FIX::IncorrectDataFormat,
                         FIX::IncorrectTagValue, FIX::UnsupportedMessageType) {
        // 1. Validate (FR-003) — reject-and-log on failure, no exceptions
        //    propagate out of this function.
        // 2. Translate FIX::Message -> OrderEventWire / CancelEventWire.
        // 3. try_push onto the inbound ring buffer.
        // 4. Return. No blocking, no OMS logic here — QuickFIX calls this
        //    on its own network thread, which is not a pinned pipeline
        //    thread and must not become one by accident.
    }
    // onCreate / onLogon / onLogout / toApp / fromAdmin / toAdmin:
    // session lifecycle only — update FixSession domain state, never touch
    // order wire structs here.
};
```

The Outbound Gateway is the mirror image: its `fromApp` handles incoming
`ExecutionReport`s from the venue and pushes `ExecReportEventWire`s onto the
OMS-Core-bound buffer; its send path pulls `OrderEventWire`s off the
outbound-routing buffer and constructs/sends FIX `NewOrderSingle` messages.

**Venue swappability (FR-033) falls out of this for free:** from the Outbound
Gateway's perspective, the simulated venue and a real broker both look like
"whatever is on the other end of this FIX session." No separate `IVenue`
interface is needed — the FIX session boundary already *is* that
abstraction.

---

## 5. Summary table

| Boundary | Kind | Mechanism |
|---|---|---|
| Pipeline stage -> stage | Hot path | `SpscRingBuffer<WireStruct, N>` |
| OMS Core -> risk rules | Hot path (debatable) | Virtual `IRiskRule`, revisit if profiling demands |
| Venue simulator / real broker | Hot path | FIX session itself (no C++ interface needed) |
| Market data source selection | Cold path | `IMarketDataFeed` |
| Capture / replay backend selection | Cold path | `ICaptureSink` / `IReplaySource` |
| Gateway <-> QuickFIX | External contract | `FIX::Application` (imposed by the library) |

**Next suggested task:** the threading/pipeline architecture itself — pinning
these ring buffers and interfaces to concrete threads, defining startup/
shutdown ordering, and deciding buffer capacities per stage.
