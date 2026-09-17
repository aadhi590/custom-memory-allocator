# custom-memory-allocator

A general-purpose, from-scratch heap memory allocator written in C++17 for
Linux (x86-64). It implements the core mechanics of a real allocator —
mmap-backed memory pools, block splitting and coalescing, size-class free
lists, and a locking progression from a single global mutex to per-thread
caching — as a portfolio project targeting systems/embedded C++ roles.

## Motivation

Most application-level C++ work never requires understanding what `malloc`
and `new` actually do underneath. This project builds that understanding
from first principles: how memory blocks are laid out and tracked, how free
space is found and reused, how fragmentation is fought via splitting and
coalescing, how alignment guarantees are upheld, and how a single-threaded
design is evolved into a thread-safe one. The goal is a small, well-tested,
well-documented codebase that demonstrates real allocator engineering
tradeoffs rather than a black-box wrapper around `malloc`.

## Current status

**All 7 phases complete.** This is the finished state of the project:
block metadata, a real `mmap`-backed general allocator with free-list
reuse, splitting/coalescing, `calloc`/`realloc`, a pooled allocation path
for small, common sizes, and thread safety with a measured concurrency
progression from a single global lock to per-thread caching.

`my_malloc`/`my_free` (`include/allocator.hpp`) route through two paths.
Small, common sizes (<= 4096 bytes, rounded up to one of 9 fixed size
classes) go to `include/memory_pool.hpp`'s `Pool`: O(1) allocation and
deallocation with zero search, zero splitting, zero coalescing. Everything
larger uses the general path: `os_memory`'s `mmap`/`munmap` wrapper, a
doubly-linked free list (`include/free_list.hpp`) searched first-fit, and
`include/block.hpp`'s `BlockHeader`/`BlockFooter` boundary-tag pair, with
splitting/coalescing and `calloc`/`realloc`. `my_free()`/`my_realloc()`
identify which path a pointer came from via a validated pool-ownership tag
(O(1) in the number of size classes, not the number of arenas) rather than
an address-range search. See [docs/memory-model.md](docs/memory-model.md)
for the full block-layout explanation plus worked examples of splitting
and coalescing, and [docs/design-decisions.md](docs/design-decisions.md)
for the reasoning behind specific choices, including the pointer-to-pool
routing decision and Phase 7's concurrency design.

Thread safety (`src/allocator.cpp`) is built as three separately
selectable stages via a compile-time `ALLOCATOR_CONCURRENCY_STAGE` macro:
a single global mutex (Stage 1), per-size-class locks plus one
general-path lock (Stage 2), and per-thread caching on top of Stage 2's
locking (Stage 3, the default). All three are exercised and compared in
`benchmarks/benchmark_allocator.cpp` — see
[Performance summary](#performance-summary) below. Caller-requested
alignment beyond the default remains out of scope (see "Cut from the
original 13-phase plan" below).

## Build instructions

Requires a C++17 compiler, CMake 3.16+, and network access on first configure
(to fetch GoogleTest via `FetchContent`). Linux x86-64 only.

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

Pass `-DCMAKE_BUILD_TYPE=Release` to `cmake -B build` for an optimized build;
Debug is the default.

### Sanitizer verification

Every phase of this project has been verified locally under sanitizers
before being pushed (see the "cut from the original 13-phase plan" note
below for why this stands in for a formal CI pipeline). To reproduce:

```sh
# AddressSanitizer + UndefinedBehaviorSanitizer (general correctness)
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan

# ThreadSanitizer (concurrency correctness -- Phase 7's test_thread_safety.cpp)
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan
ctest --test-dir build-tsan
```

ASan and TSan are built and run separately (as separate `build-asan`/
`build-tsan` directories), never combined in one binary, since the two
sanitizers generally aren't compatible with each other. As of Phase 7, the
full suite (136 tests) passes clean under both, including zero data races
reported by TSan across all of `test_thread_safety.cpp`'s multithreaded
tests -- and every test that existed before Phase 7 (129 of the 136) was
left completely unmodified by adding concurrency support; only new tests
were added.

## Performance summary

Full methodology, environment details, and honest discussion of surprising
results are in [docs/benchmarks.md](docs/benchmarks.md). Headline numbers,
measured on this machine (WSL2, Intel i5-7200U, 2 physical/4 logical
cores):

- **Single-threaded**: this allocator's `malloc` mean latency is ~1135ns
  vs. glibc's ~1006ns on the same mixed-size workload (this allocator
  trails glibc here — reported honestly, with the likely reasons, in
  `docs/benchmarks.md`).
- **Multithreaded throughput, 1 -> 8 threads** (the single most important
  chart in the project): Stage 1 (global mutex) collapses from
  30.1M to 4.4M ops/sec under contention, while Stage 2 (per-pool locks)
  and Stage 3 (thread-local caching) hold up far better, and Stage 3 is
  nearly 2x Stage 1/2's throughput at 1 thread (58.7M ops/sec) by
  amortizing lock cost across a batch of 32 allocations.

## Repository layout

```
include/     public headers (block.hpp, allocator.hpp, free_list.hpp, os_memory.hpp, memory_pool.hpp have real content)
src/         implementation files (allocator.cpp, free_list.cpp, os_memory.cpp, memory_pool.cpp have real content)
tests/       GoogleTest test suites
benchmarks/  throughput/latency and fragmentation-awareness benchmarks (Phase 7)
examples/    example programs using the allocator (basic_allocation.cpp)
docs/        architecture, memory model, design decisions, benchmark results
scripts/     developer tooling (run_benchmarks.sh)
```

## Roadmap

**The project plan was trimmed from an original 13 phases to 7, partway
through, for time.** Phases 0-5 below are unchanged from the original plan.
Phase 7 is a merged final phase covering what the original plan spread
across separate concurrency, benchmarking, hardening, and polish phases.
See the "Scope reduction: 13 phases to 7" entry in
[docs/design-decisions.md](docs/design-decisions.md) for the full
rationale and exactly what was cut. **All 7 phases are now complete.**

- [x] **Phase 0** — Project scaffolding: repo layout, CMake + GoogleTest
      build, licensing, tooling config.
- [x] **Phase 1** — Block metadata: `BlockHeader` layout, bit-packed
      size/`is_free`, free-list pointer overlay, boundary-tag footer concept.
- [x] **Phase 2** — Basic bump allocator: `os_memory` mmap wrapper, real
      `my_malloc`/`my_free` backed by a single growing region.
- [x] **Phase 3** — Free list: reuse freed blocks via an intrusive free list
      instead of only bumping forward.
- [x] **Phase 4** — Splitting and coalescing: carve oversized free blocks
      down to size, merge adjacent free blocks using boundary tags.
- [x] **Phase 5** — `my_calloc`/`my_realloc`: overflow-checked, always-zeroed
      calloc; realloc with in-place growth/shrink via Phase 4's
      splitting/coalescing where possible, allocate+copy+free otherwise.
- [x] **Phase 6** — Memory pools / size classes: fixed-size slot pools for
      small, common allocation sizes as a second, O(1) allocation path
      alongside the general free-list allocator (which continues to handle
      everything above the largest size class).
- [x] **Phase 7** — Concurrency and performance benchmarking (merged final
      phase): a locking progression for thread safety (global mutex ->
      per-pool locks -> thread-local caching, all three kept selectable
      and benchmarked), ThreadSanitizer-verified concurrency tests, and
      real, honestly-reported throughput/latency/fragmentation benchmarks.

### Cut from the original 13-phase plan

These were explicitly part of the original plan and are now out of scope
for this project rather than silently dropped:
- Caller-requested alignment beyond the default (originally slated as its
  own phase before the plan was trimmed).
- A formal CI / GitHub Actions workflow.
- A separate stress-testing phase (folded into ordinary test-suite work
  throughout, rather than a dedicated phase).
- A separate performance-optimization-pass phase distinct from Phase 7's
  benchmarking.
- The full 9-workload/formal-fragmentation-metrics benchmark suite
  originally envisioned — Phase 7's benchmarking is scoped down from this.

## License

MIT — see [LICENSE](LICENSE).
