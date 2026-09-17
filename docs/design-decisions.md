# Design Decisions

A running log of non-obvious engineering decisions in this project, in the
order they were made. Each entry states the decision, the alternatives
considered, and why the chosen option won.

---

## 1. Why `mmap` over `sbrk` for obtaining memory from the OS

**Decision**: `os_memory.hpp`/`.cpp` (Phase 2+) will request memory from the
kernel via `mmap(2)`, not `sbrk(2)` / `brk(2)`.

**Alternatives considered**:
- `sbrk`/`brk`: grow a single contiguous data-segment break pointer.
- `mmap`: request independent, arbitrarily-sized, arbitrarily-freeable
  virtual memory regions.

**Reasoning**:

- **Independent release.** `sbrk` only supports shrinking the heap from its
  *current end* — you can't return the memory backing an allocation in the
  middle of the heap to the OS without also giving back everything after it.
  `mmap`/`munmap` operate on arbitrary regions, so a large allocation (or an
  entire arena, in the pool-based design from Phase 6 onward) can be returned
  to the OS independently of every other allocation. For an allocator meant
  to demonstrate real design tradeoffs, this matters more than raw
  simplicity.
- **No single shared mutable break pointer.** `sbrk` manipulates one global
  program break. Every thread doing an `sbrk`-based low-level allocation is
  contending over the same pointer, and getting concurrent `sbrk` calls right
  is notoriously easy to get wrong. `mmap` calls are independent syscalls
  with no shared mutable OS-level state to serialize on, which fits the
  eventual multi-threaded design (Phase 8) far better.
- **Large-allocation behavior for free.** Because `mmap` regions are
  independently addressed and unmapped, very large allocations can bypass
  the block/free-list machinery entirely later (mirroring what glibc's
  `malloc` does above `mmap_threshold`) without any special-casing at the
  `sbrk`-break level.
- **Linux-only target is explicit.** This project is scoped to Linux x86-64
  (see README), so there's no portability motivation to prefer `sbrk`'s wider
  historical Unix support. `mmap` is the modern, well-documented interface
  Linux allocators are actually built on.
- **Cost accepted**: `mmap` has higher per-call overhead than `sbrk` (it's a
  full syscall that manipulates page tables, versus `sbrk`'s cheaper break
  adjustment) and normally rounds up to page granularity (4 KiB). This is
  fine because the allocator requests memory in large chunks (arenas) and
  then subdivides them internally via the block/free-list layers — the
  syscall cost is amortized across many small allocations, not paid per
  `my_malloc` call.

---

## 2. Why pack `is_free` into the size field's low bit instead of a separate `bool`

**Decision**: `BlockHeader` stores `is_free` as bit 0 of the same `size_t`
that holds the block size, via `get_size()`/`set_size()`/`is_free()`/
`set_free()` helper methods, rather than as a standalone `bool is_free`
member.

**Alternatives considered**:
- A separate `bool is_free` data member alongside `size_t size`.
- The bit-packed single `size_t` (chosen).

**Reasoning**:

- **The low bits are provably free.** Every block size this allocator ever
  stores is rounded up to a multiple of `alignof(std::max_align_t)` (16 bytes
  on x86-64) before being written into a header, as a hard invariant of the
  allocation logic. That means the low 4 bits of any valid stored size are
  always zero — there is no scenario where a legitimate size value needs bit
  0. Reclaiming that bit isn't a hack layered on top of unrelated data; it's
  using bits that are structurally guaranteed to be unused.
- **Header size stays a clean multiple of the alignment.** A separate
  `bool is_free` next to `size_t size` would, after the compiler's natural
  alignment padding, very likely still round up to the same total struct
  size on a typical x86-64 ABI (a `bool` next to an 8-byte `size_t` gets
  padded to another 8 bytes anyway) — so in practice this decision doesn't
  even save space in the *current* single-field header. The real payoff is
  future-proofing: as more fields are added to `BlockHeader` in later phases
  (e.g., an owning-pool identifier, a magic/canary value for corruption
  detection), keeping flags bit-packed into existing fields avoids
  accumulating separate bool members that each cost padding and each need to
  be threaded through every place the header is copied, zeroed, or compared.
- **Every allocator header this project is modeled after does this.** Bit-
  stealing flags into size fields (using the guaranteed-zero low bits from
  alignment) is standard practice in real allocator implementations
  (dlmalloc-family allocators, for instance, pack multiple flag bits into the
  size field of their boundary tags). Implementing it here, correctly and
  with round-trip tests, is part of what makes this a portfolio piece that
  demonstrates real allocator internals rather than a toy.
- **Cost accepted**: raw bit manipulation is less readable than `header.free`
  at the call site, and it's easy to get wrong (forgetting to mask the flag
  bit back out when reading size, or clobbering it when writing size). This
  is mitigated by centralizing all bit manipulation inside
  `get_size()`/`set_size()`/`is_free()`/`set_free()` — no other code in the
  allocator is allowed to touch `size_and_flags_` directly — and by the
  round-trip tests in `tests/test_block_layout.cpp` that exercise the helpers
  across representative sizes and free/allocated states.

---

## 3. Why first-fit instead of best-fit for the free list search

**Decision**: `FreeList::find_first_fit()` (Phase 3) walks the free list
from the head and returns the first block whose size is large enough,
rather than scanning the whole list to find the smallest block that still
fits ("best-fit").

**Alternatives considered**:
- **First-fit** (chosen): return the first sufficiently-large block
  encountered during the walk.
- **Best-fit**: scan every free block and return the smallest one that
  still satisfies the request.
- **Next-fit**: a first-fit variant that resumes searching from wherever
  the previous search left off, instead of always restarting at the head.

**Reasoning**:

- **Simplicity first.** First-fit is the simplest correct search strategy
  for an unsegregated free list, and it's the natural starting point before
  later phases add size-class segregation (Phase 6) or other structural
  changes that make the search strategy question worth revisiting from
  scratch.
- **Avoids paying for a full list walk on every allocation.** Best-fit
  guarantees it will find the smallest sufficient block, but doing so
  requires visiting every free block on every single call, even when an
  early block would have worked fine. First-fit can return as soon as it
  finds any sufficient block.
- **The real tradeoff needs data, not guesses.** Best-fit's actual
  advantage — less wasted space per allocation, because it avoids handing
  out an oversized block when a tighter one exists — is a genuine
  fragmentation win in some workloads. But whether that win is worth its
  search cost depends on real allocation patterns this project doesn't have
  benchmark data for yet. Deciding between first-fit, best-fit, and
  next-fit from first principles instead of measurement would be guessing;
  that comparison is deferred until there's a benchmarking harness
  (Phase 9).
- **Cost accepted**: first-fit is known to be more prone to leaving small,
  hard-to-reuse fragments near the head of the list than best-fit is,
  which is exactly what Phase 4's splitting and coalescing exist to
  mitigate (splitting avoids handing out an oversized block whole in the
  first place; coalescing merges adjacent small free fragments back
  together). The two phases are complementary: first-fit's weakness is
  addressed by the block-management logic around it, not by the search
  strategy itself.

See also `docs/architecture.md`, which documents this same decision at the
system-diagram level for a reader working top-down through the layers.

---

## 4. Scope reduction: 13 phases to 7

**Decision**: partway through the project (immediately before Phase 6), the
original 13-phase plan was deliberately trimmed to 7 total phases, for time.
This is a scope decision, not an abandonment — everything below was
explicitly considered and cut, not silently dropped or forgotten.

**What changed**: Phases 0-5 are unchanged from the original plan (project
scaffolding through `calloc`/`realloc`). Phase 6 (memory pools / size
classes) is unchanged in substance from the original plan's memory-pools
phase. Everything the original plan spread across the remaining phases —
concurrency/locking, thread-local caching, performance benchmarking,
hardening, a CI phase, and a final polish phase — is merged into a single
Phase 7: "Concurrency and performance benchmarking."

**What was cut, specifically, rather than silently dropped**:
- **Caller-requested alignment beyond the default.** Originally its own
  phase. The general-path allocator already guarantees
  `alignof(std::max_align_t)` alignment for every allocation (see
  `docs/memory-model.md`); supporting caller-requested *stricter*
  alignments (e.g. for SIMD types) is a real, well-scoped feature, but not
  one that fits the remaining time budget.
- **A formal CI / GitHub Actions workflow.** Every phase in this project
  has been built and verified locally (including under
  `-fsanitize=address,undefined`) before being pushed, which covers the
  correctness goal a CI pipeline would otherwise exist to enforce; automating
  that enforcement on every push is being cut as a distinct phase.
- **A separate stress-testing phase.** Stress-style tests (e.g. the
  ~600-allocation arena-rollover test from Phase 2, the mixed-routing test
  from Phase 6) are written inline as part of each phase's normal test
  suite instead of being reserved for a dedicated later phase.
- **A separate performance-optimization-pass phase**, distinct from
  measuring performance at all. Phase 7 benchmarks the allocator as it
  stands; a follow-up phase specifically dedicated to acting on those
  numbers (profiling and tuning hot paths) is cut.
- **The full 9-workload/formal-fragmentation-metrics benchmark suite**
  originally envisioned for the benchmarking phase. Phase 7's benchmarking
  is scoped down to comparing the allocator's own before/after numbers
  (e.g. pooled vs. general-path allocation latency, lock-contention
  behavior) rather than a large, formally-designed workload suite with
  fragmentation-metric tracking.

**Reasoning**: this project's goal (per the README's Motivation section) is
demonstrating real allocator engineering tradeoffs for a systems/embedded
C++ portfolio, not shipping a production-grade allocator. The core
learning content — block layout, free lists, splitting/coalescing,
calloc/realloc, size-class pools, and a concurrency story with
benchmarking to back it up — is fully covered by the 7-phase plan. The cut
items are real, valuable engineering work, but they extend the project's
breadth (more features, more infrastructure) rather than its depth on the
allocator internals the project is actually about, and cutting them now
means the phases that remain get done well rather than the full list being
rushed.

---

## 5. Pointer-to-pool routing: a validated speculative tag, not an arena range check

**Decision**: `my_free()`/`my_realloc()` determine whether a pointer came
from a pool by speculatively reading a `Pool*` tag from the pointer's
presumed header location and validating it against the small, fixed set
of known `Pool` object addresses (`is_known_pool()`), rather than checking
the pointer's address against each pool's tracked arena ranges.

**Alternatives considered**:
- **Per-pool arena range check**: for each pool, check whether `ptr` falls
  within any of that pool's tracked arena address ranges. Cost grows with
  the total number of arenas across all pools -- O(total pool arenas),
  worst case checked one pool at a time.
- **Validated speculative tag** (chosen): every pooled slot's header
  stores a `Pool*` identifying its owning pool (`PoolSlotHeader`, see
  `memory_pool.hpp`). `my_free()` reads that field unconditionally and
  checks whether it equals the address of one of the 9 statically-known
  `Pool` objects.

**Confirmed exact-equality check, not a range check**: `is_known_pool()`
(`src/allocator.cpp`) does `&pool == candidate` for each of the 9 `Pool`
objects in the fixed-size `pools()` array -- pointer *equality* against a
small, enumerable set of specific addresses, never an inequality/range
comparison (`>=`/`<`) against any address span. This distinction matters:
a range check only needs a value to fall *somewhere* between two bounds,
which a coincidentally-large general-path size could conceivably do if
this allocator ever handled multi-gigabyte allocations sized to land
inside that range. An exact-equality check against a handful of specific
addresses has no such weakness -- a general-path size would have to
numerically equal one particular static address bit-for-bit, not merely
fall within a span, which is the basis for the "no false positives, by
construction" claim below.

**Reasoning**:

- **O(kNumSizeClasses), not O(arenas or allocations).** `kNumSizeClasses`
  is a compile-time constant (9). Validating a tag against 9 known
  addresses costs the same whether the allocator has acquired 2 arenas or
  2000 -- it depends only on how many size classes exist, never on how
  much memory has been requested from the OS or how many allocations are
  live. An arena range check's cost, by contrast, grows with exactly those
  things. Since Phase 7 benchmarks allocation/deallocation latency
  directly, this asymptotic difference is the whole point of choosing it.
- **No false positives, by construction, not by luck.** A general-path
  block's header stores a small `size_t` (at most a few GiB in any
  realistic allocation). A `Pool` object's address is a static-storage-
  duration address in the program's data segment. These two value spaces
  don't overlap in practice -- a real allocation size can never
  numerically equal one of the 9 known `Pool` addresses -- so reading the
  tag speculatively and validating it is exact for this allocator's
  threat model (legitimate callers passing pointers this allocator
  actually returned), not merely "astronomically unlikely to collide."
- **Doesn't touch the general path's `BlockHeader` format.** An
  alternative that embeds a discriminant tag directly in `BlockHeader`
  (so every block, pooled or general, is self-describing) was considered
  and rejected: it would mean modifying the already-established,
  already-tested Phase 1-5 header format purely to serve Phase 6's
  routing problem, trading a stable, unchanged general path for a
  speculative-tag design that lives entirely in the new code instead.
- **Cost accepted**: this technique relies on the "no false positives in
  practice" reasoning above rather than a type-safe discriminant, which is
  a weaker guarantee in the abstract (though not in this allocator's
  actual usage). A defensive `assert` in both `my_free()` and
  `my_realloc()` (compiled out under `NDEBUG`, like the rest of this
  codebase's invariant checks) catches the other failure mode -- a
  pointer that's neither a recognized pool tag nor within a tracked
  general-path arena -- so a genuinely invalid pointer is caught loudly in
  debug builds rather than silently misinterpreted either way.

---

## 6. Concurrency staging: a compile-time macro and `if constexpr`, not a runtime switch

**Decision**: which locking strategy `src/allocator.cpp` uses (a single
global mutex, per-pool locks, or thread-local caching) is selected by the
preprocessor macro `ALLOCATOR_CONCURRENCY_STAGE` (1, 2, or 3; default 3),
read into `constexpr int kConcurrencyStage`, and dispatched on via
`if constexpr`. Three separate benchmark executables
(`benchmark_allocator_stage1/2/3`) are built from the *same* source file
by giving each its own `target_compile_definitions` in `CMakeLists.txt`,
rather than one executable that picks its stage at runtime.

**Alternatives considered**:
- **Runtime switch** (an enum/int checked at the top of every hot-path
  function, or via a virtual `LockingStrategy` interface): would allow a
  single built binary to be reconfigured without recompiling, and would
  make it easier to, say, change strategy mid-process. Rejected for this
  project because (a) Part B's benchmarks need to compare genuinely
  different compiled code paths, not one binary branching on a runtime
  flag -- a runtime `if` would add a real branch-and-maybe-indirect-call
  cost to *every* stage's hot path, including Stage 1's, which would then
  be measuring "Stage 1 plus dispatch overhead" rather than Stage 1 alone;
  and (b) this allocator's usage pattern (fixed at process startup, not
  changed while running) never needs runtime reconfigurability.
- **Compile-time macro + `if constexpr`** (chosen): `kConcurrencyStage` is
  a `constexpr int`, so `if constexpr (kConcurrencyStage == 3) { ... }
  else { ... }` compiles away the untaken branch entirely -- Stage 1's
  binary contains zero thread-local-cache code, and vice versa. This is
  exactly what Part B's throughput comparison needs: three binaries that
  differ only in the specific mechanism being measured, with no shared
  runtime dispatch overhead polluting any of them.

**Note on well-formedness**: `if constexpr` still requires both branches to
be valid C++ regardless of which one is compiled in for a *given* build
(unlike `#ifdef`, which can contain code that doesn't even parse under the
other configuration) -- so both the locked and thread-local-cache code
paths had to be written as ordinary, always-parseable C++, with only the
macro deciding which one a given executable actually contains after
compilation. This is a stricter but safer form of compile-time
configuration than raw preprocessor branching would have been.

---

## 7. Thread-local cache batch size and cap: 32 and 64

**Decision**: `kThreadLocalCacheBatchSize = 32` (how many slots a
Stage 3 cache refills from its pool at once, on a cache-empty `malloc`)
and `kThreadLocalCacheCap = 2 * kThreadLocalCacheBatchSize = 64` (the
count above which a cache flushes a batch back to its pool, on a
cache-full `free`).

**Reasoning**:
- **Batch size trades off lock-amortization against wasted memory.** A
  larger batch means fewer pool-mutex acquisitions per allocation (better
  amortization -- this is the entire reason Stage 3's 1-thread throughput
  in `docs/benchmarks.md` is nearly double Stage 1/2's), but also means
  more slots sitting idle in one thread's cache, unavailable to any other
  thread, after a burst of allocation followed by a burst of frees
  elsewhere. 32 was chosen as a round, moderate number -- large enough to
  amortize a `std::mutex` lock/unlock pair over enough operations that its
  fixed cost per allocation becomes small, small enough that no single
  thread can sequester more than a few KB-worth of slots per size class at
  once for the pool sizes this project uses.
- **The cap is set to exactly twice the batch size.** This is the simplest
  value that guarantees the cap-triggered flush-back path is reachable at
  all without immediately re-triggering a refill: after a flush, the cache
  drops by one batch's worth, landing well below the cap again rather than
  oscillating between "just flushed" and "immediately needs to flush
  again" on the next single free.
- **Both are ordinary named `constexpr` constants in `src/allocator.cpp`,
  not tuned per-size-class.** Every size class uses the same batch size
  and cap. A per-size-class tuning pass (e.g. smaller batches for larger
  slot sizes, to bound worst-case idle memory in bytes rather than in slot
  count) was considered and explicitly deferred as out of scope -- it
  would require empirical tuning this project's benchmarking scope doesn't
  cover, for a benefit that's speculative without that data.

---

## 8. Cross-thread free: always push to the freeing thread's own cache

**Decision**: in Stage 3, `my_free()` on a pooled pointer always pushes
the freed slot onto the *freeing* thread's thread-local cache -- never the
allocating thread's cache, and never directly back to the shared `Pool` --
regardless of which thread originally allocated that slot. The only
exception is the cap: if the freeing thread's cache is already at
`kThreadLocalCacheCap`, a batch is flushed back to the shared pool (under
that pool's mutex) to make room, same as it would be for a same-thread
free.

**Alternatives considered**:
- **Return cross-thread frees to the global pool directly** (i.e. detect
  "this slot wasn't allocated by me" and skip the local cache entirely for
  those frees): keeps each thread's cache purely a reflection of its own
  allocation activity, at the cost of extra bookkeeping (the slot would
  need to record which thread -- or at least "was it ever cached
  elsewhere" -- to distinguish this case) and an extra lock acquisition on
  every cross-thread free, defeating the point of the cache for exactly
  the workload this project's tests explicitly exercise (a producer thread
  allocating and a consumer thread freeing).
- **Always push to the freeing thread's own cache** (chosen): no
  bookkeeping beyond what every free already needs; a cross-thread free is
  exactly as cheap as a same-thread one. The trade-off, documented
  honestly rather than hidden: a thread whose job is mostly to free memory
  other threads allocated (a dedicated "consumer"/cleanup thread, say) can
  accumulate cached slots for pools it may rarely or never personally
  allocate from again. This is bounded, not unbounded -- `kThreadLocalCacheCap`
  (Design Decision 7) applies uniformly regardless of *why* a cache grew,
  so the worst case is that consumer thread's cache sits at the cap
  per size class until it exits (at which point, per the current design,
  those cached slots are simply left for `allocator_shutdown()` to handle,
  same as any other thread's leftover cache) -- not that memory is
  permanently lost or that the cap is bypassed.
- **Verified by a dedicated test**: `CrossThreadFreeProducerAllocatesConsumerFrees`
  in `tests/test_thread_safety.cpp` specifically allocates on one thread,
  hands pointers to a second thread via a queue, and frees them all on
  that second thread, run clean under ThreadSanitizer -- this scenario is
  exercised deliberately, not just handled incidentally by the general
  mixed-workload test.

---

## Known issues / deferred hardening

Items identified during the Phase 1 → Phase 2 review of `block.hpp`. Items 1
and 2 were resolved in Phase 3, once free-list code existed to make the
questions concrete; items 3 and 4 remain deliberately deferred.

1. **RESOLVED (Phase 3): `FreeListLinks` now uses `BlockHeader*` instead of
   `void*`.** The open question was whether `BlockHeader*` could even be
   named inside `FreeListLinks`, which is defined above `BlockHeader` in
   `block.hpp`. It can: a forward declaration (`class BlockHeader;`) placed
   before `FreeListLinks` is sufficient, because a pointer's size and
   representation don't depend on the pointee type being complete -- only
   *dereferencing* the pointer requires completeness, and nothing does that
   until `free_list.cpp` (which includes the full `block.hpp` and therefore
   sees the complete `BlockHeader` class). `void*` was not an intentional
   design choice; it was simply what Phase 1 wrote before there was any
   free-list code to prove out the alternative. Switching to `BlockHeader*`
   removes a `reinterpret_cast` from every free-list traversal step and lets
   that code call `is_free()`/`get_size()`/etc. directly on list nodes.

2. **RESOLVED (Phase 3): `payload()` and `free_list_links()` now assert
   their invariants.** `payload()` asserts `!is_free()`;
   `free_list_links()` asserts `is_free()`. Both compile out in release
   builds (`NDEBUG`), matching how the rest of the codebase treats
   `<cassert>` assertions -- these are debug-time bug detectors, not
   input validation. Implementing this required splitting out a private,
   assertion-free `data_address()` helper that both accessors call
   internally: `free_list_links()` needs to compute the same address
   `payload()` does, and it needs to do so on a block that is free (i.e.
   exactly the block state where `payload()`'s assertion would fire), so
   the two accessors can no longer share the address computation directly
   without tripping each other's guard.

3. **`set_size()` silently masks off an already-set flag bit instead of
   asserting the precondition.** The comment above `set_size()` states the
   precondition that the caller's size must not already have bit 0 set, but
   the code doesn't check it — it just masks the bit off unconditionally. A
   caller that accidentally passes an odd size (e.g. a size that wasn't
   alignment-rounded) won't get a signal that something upstream is wrong;
   the value is quietly truncated instead. Consider `assert((size & 0x1) ==
   0)` once there's a debug-assertion story for the project generally.

4. **`BlockHeader`'s constructor defaults `free = true`.** A `BlockHeader{size}`
   constructed without an explicit second argument is free by default. It's
   worth reconsidering whether defaulting to *allocated* (`free = false`)
   is the safer failure mode: if calling code ever forgets to pass the
   argument at a callsite that meant to construct an allocated block, the
   current default fails open (the block looks free and reusable) rather
   than failing closed (the block looks allocated and gets left alone until
   someone deliberately frees it). Revisit alongside item 2 above.
