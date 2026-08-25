# Low-Level Design — Data Structures & Algorithms

**Document ID:** DESIGN-004
**Status:** Draft v1
**Traces to:** 03_API_DESIGN.md (hot-path wire API), 04_HIGH_LEVEL_ARCHITECTURE.md (thread layout)

This is where the "low-level systems learning" goal from the Charter actually
gets cashed in. Every section below is a concrete implementation decision,
not a restatement of the interface — the point is to get specific enough
that writing the code is mostly transcription.

---

## 1. `SpscRingBuffer` — concrete memory layout

The interface was fixed in 03_API_DESIGN.md. The part that matters for
p99.9 is the memory layout underneath it — specifically, avoiding **false
sharing** between the producer and consumer.

```cpp
template <typename T, size_t Capacity>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of 2 (enables mask instead of modulo)");
    static constexpr uint64_t mask_ = Capacity - 1;

    // head_ is written only by the producer, read occasionally by the
    // consumer. tail_ is the mirror image. If these two atomics share a
    // cache line, every producer write invalidates the consumer's cached
    // copy and vice versa — the two threads end up fighting over one
    // cache line via MESI, which silently caps your tail latency without
    // showing up anywhere in the code as an obvious bug.
    alignas(64) std::atomic<uint64_t> head_{0};
    char pad_head_[64 - sizeof(std::atomic<uint64_t>)];

    alignas(64) std::atomic<uint64_t> tail_{0};
    char pad_tail_[64 - sizeof(std::atomic<uint64_t>)];

    alignas(64) std::array<T, Capacity> buffer_;

public:
    bool try_push(const T& item) noexcept {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= Capacity) return false;          // full
        buffer_[h & mask_] = item;                     // write payload first
        head_.store(h + 1, std::memory_order_release);// then publish
        return true;
    }

    bool try_pop(T& out) noexcept {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        const uint64_t h = head_.load(std::memory_order_acquire);
        if (t == h) return false;                      // empty
        out = buffer_[t & mask_];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
};
```

Key decisions worth being able to explain, not just paste:
- **Power-of-2 capacity + bitmask indexing** instead of modulo — a modulo by
  a non-power-of-2 is a division instruction; a bitmask AND is one cycle.
- **`release`/`acquire`, not `seq_cst`.** Sequential consistency forces a
  full memory fence on every operation, which you don't need for a single
  producer / single consumer pair — release-on-publish, acquire-on-consume
  is exactly the ordering guarantee required (the payload write happens-before
  the index becomes visible) and nothing stronger.
- **Write-then-publish.** The payload is written into `buffer_` *before* the
  index is advanced. If this were reversed, the consumer could observe the
  new index and read a slot that hasn't been written yet.

---

## 2. Order state machine — table-driven, not `if`/`else`

FR-011/FR-012 require every `(state, event)` pair to have an explicit,
testable outcome. The only way to make "explicit and exhaustive" a property
of the code itself (not just a testing discipline you have to remember) is
to make the transition table a data structure you can iterate.

```cpp
enum class OrderState : uint8_t {
    PendingNew, New, PartiallyFilled, Filled,
    PendingCancel, Cancelled, PendingReplace, Replaced,
    Rejected, Expired, DoneForDay,
    Count // sentinel, gives you the array size for free
};

enum class OrderEvent : uint8_t {
    Ack, PartialFill, FullFill, CancelRequest, CancelAck,
    ReplaceRequest, ReplaceAck, RejectEvent, ExpireEvent,
    Count
};

// INVALID means: this (state, event) pair must produce a Reject/CancelReject
// response, not a state change. It's a real, defined outcome — not an
// unhandled case.
inline constexpr auto kTransitionTable = [] {
    std::array<std::array<OrderState, (size_t)OrderEvent::Count>,
               (size_t)OrderState::Count> table{};
    for (auto& row : table) row.fill(OrderState::Rejected); // default: INVALID
    // Explicitly populate the legal transitions:
    table[(size_t)OrderState::PendingNew][(size_t)OrderEvent::Ack] = OrderState::New;
    table[(size_t)OrderState::New][(size_t)OrderEvent::PartialFill] = OrderState::PartiallyFilled;
    table[(size_t)OrderState::New][(size_t)OrderEvent::FullFill] = OrderState::Filled;
    table[(size_t)OrderState::New][(size_t)OrderEvent::CancelRequest] = OrderState::PendingCancel;
    table[(size_t)OrderState::PendingCancel][(size_t)OrderEvent::CancelAck] = OrderState::Cancelled;
    // ... remaining legal transitions, enumerated the same way.
    return table;
}();
```

**Why default-to-Rejected instead of leaving it unset:** an unset entry in a
hand-written `if`/`else` chain is silent — nothing happens, and you find out
in production when an order gets stuck. A table that defaults every cell to
"reject" and then explicitly carves out the legal transitions makes the
*unhandled* case the safe case. FR-012's unit test becomes trivial: iterate
all `(state, event)` pairs, assert each is either a documented legal
transition or `Rejected` — there's no third option the test could miss.

---

## 3. Order object pooling — no heap allocation mid-session

The `Order` *domain* object (richer than the `OrderEventWire` from
03_API_DESIGN.md — it owns its full `ExecutionReport` history) lives entirely
inside the OMS Core thread. Per NFR-003, it must not be heap-allocated
per-order at runtime.

```cpp
class OrderPool {
    std::vector<Order> slots_;   // reserved once at startup, never resized
    size_t next_free_ = 0;
public:
    explicit OrderPool(size_t capacity) { slots_.reserve(capacity); }

    // order_id IS the index — O(1) lookup, no hashing, no pointer chasing.
    std::optional<uint64_t> allocate() {
        if (next_free_ >= slots_.capacity()) return std::nullopt; // pool exhausted
        slots_.emplace_back();
        return next_free_++;
    }
    Order& get(uint64_t order_id) { return slots_[order_id]; }
};
```

Pool exhaustion is a **defined, tested failure mode** (reject new orders with
a specific reason, per FR-024's "every rejection is attributable" principle)
rather than an unbounded `std::vector` growth or an OOM crash. Capacity is a
generous, explicit constant, sized the same way ring buffer capacities are
(§5 of 04_HIGH_LEVEL_ARCHITECTURE.md) — a documented placeholder to be tuned
once real order-volume data exists.

---

## 4. FIX parsing — where "zero-copy" honestly starts and stops

Worth being direct about a limitation here rather than glossing over it:
QuickFIX/C++ (chosen in 01_REQUIREMENTS.md to avoid reinventing the session
layer) does its own internal parsing into a `FIX::Message` object, which is
not a zero-copy, allocation-free structure. That cost is already paid before
your code sees the message.

What *is* under your control, and where the zero-copy discipline actually
applies, is the translation step from `FIX::Message` into the wire structs:

- Use `message.getField(tag, buf, len)`-style fixed-buffer accessors where
  QuickFIX offers them, writing directly into the `char[N]` fields of
  `OrderEventWire` etc., instead of `getField(tag)` overloads that return a
  `std::string`.
- No intermediate `std::string`/`std::stringstream` construction anywhere in
  the `fromApp`/translation path.

If you later decide the QuickFIX-imposed parsing cost is unacceptable
against your p99.9 target, the documented escape hatch is the
hand-rolled-FIX-engine option that was explicitly deferred in the original
scoping interview — worth remembering that door was left open on purpose,
not closed.

---

## 5. Timestamping discipline

- Single clock source throughout: `clock_gettime(CLOCK_MONOTONIC)`, never
  mixed with wall-clock time. Stage-to-stage latency is a *delta* between
  two timestamps — if they aren't from the same clock, the delta is
  meaningless.
- Timestamp captured as early as practical on each side: for inbound, at
  the start of `fromApp` (the earliest point your code observes the
  message, given QuickFIX owns everything before that); for the hot-path
  handoff between pipeline stages, at the point of `try_push`, not before
  any translation work.
- `mono_ts_ns` is part of every wire struct (03_API_DESIGN.md §2.1)
  specifically so latency can be reconstructed *after the fact* from
  captured data, not just observed live.

---

## 6. Latency histogram — hot-path recording, cold-path reporting

Recording must be cheap enough to sit directly on the hot path (NFR-001);
reporting (percentile computation, NFR-021) does not.

- Each pinned thread owns its **own** histogram instance — no cross-thread
  contention, no synchronization needed to record a sample.
- Log-linear bucketing (HDR-histogram style): fine-grained buckets at low
  latencies, coarser at high — fixed-size array, no dynamic allocation,
  bucket index computed directly from the sample value (e.g. via bit-length
  of the value for the coarse bucket, sub-divided linearly within it).
- Recording a sample is one array increment. Computing p50/p99/p99.9/p99.99
  (NFR-001) happens on a separate reporting path that reads a periodically
  swapped-out copy of the histogram — never on the thread that's recording.

---

## 7. Position ledger — array-indexed, not hash-mapped

Per 02_CORE_ENTITIES.md, `Instrument` is reference data loaded at startup —
which means the full set of tradeable symbols is known before the hot path
ever runs. That makes a `std::unordered_map<symbol, Position>` an
unnecessary hot-path cost (hashing + collision handling on every fill).

Instead: resolve `symbol -> instrument_index` once at startup, and hold
positions in `std::vector<Position>` indexed directly by that integer.
Updating a position on a fill becomes `positions_[instrument_index]`, no
hashing involved.

---

## 8. Risk check evaluation loop

```cpp
RiskDecision evaluate_all(const OrderEventWire& order,
                           const std::vector<std::unique_ptr<IRiskRule>>& rules) {
    for (const auto& rule : rules) {
        auto decision = rule->evaluate(order);
        if (!decision.passed) return decision;   // first failure wins, short-circuit
    }
    return {true, 0};
}
```

Evaluation order is fixed and configuration-defined, evaluated strictly
sequentially on the single OMS Core thread. This isn't just simplicity for
its own sake — it's required by NFR-030 (deterministic replay): if rule
evaluation order could vary (e.g. from parallelizing rules across threads),
replaying the same captured session could legitimately produce a different
risk decision on a different run, which breaks the "byte-identical resulting
state" guarantee outright.

---

## 9. Capture record — binary log format

For fast, low-overhead replay (FR-051), the capture log is a flat binary
format, not a text/JSON log:

```
[8 bytes]  mono_ts_ns
[4 bytes]  session_id (packed with a 1-bit direction flag)
[4 bytes]  raw_fix_len
[N bytes]  raw FIX message bytes
```

Written sequentially by the dedicated capture-writer thread
(04_HIGH_LEVEL_ARCHITECTURE.md §2) using buffered writes — **not** an
`fsync` per message, which would make the writer thread a throughput
bottleneck for no real benefit here. Durability is periodic/batched; the
explicit trade-off being accepted is a small window of potential data loss
on a hard crash, which is acceptable for a replay/benchmark log and would
not be acceptable for, say, a regulatory trade blotter — worth remembering
if this component is ever repurposed.

---

**Next suggested task:** the FIX message/field dictionary — the concrete
tag-by-tag mapping between FIX message types and the wire structs defined
in §1 to §9 above (this was flagged as the natural next step at the end of
04_HIGH_LEVEL_ARCHITECTURE.md too).
