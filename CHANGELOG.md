# Changelog

All notable changes to this project are documented in this file. Format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added — Phase 0: Project scaffolding
- Repository layout (`include/`, `src/`, `tests/`, `benchmarks/`, `examples/`,
  `docs/`, `scripts/`, `.github/workflows/`) with stub headers/sources for
  phases 2 through 9.
- CMake build (C++17, `-Wall -Wextra -Wpedantic`, Debug/Release default,
  GoogleTest via `FetchContent`, CTest integration).
- MIT `LICENSE`, `.gitignore`, `.gitattributes` (LF line endings), and
  `.clang-format` (LLVM-based, 100 column).

### Added — Phase 1: Block metadata
- `BlockHeader` (`include/block.hpp`): bit-packed `size`/`is_free` field,
  `FreeListLinks` overlay for free-list `next`/`prev` pointers vs. user
  payload, `BlockFooter` boundary-tag type (not yet wired into allocation
  logic), and `static_assert`s guaranteeing header size/alignment keep the
  payload address automatically aligned.
- `tests/test_block_layout.cpp`: runtime sizeof/alignof checks, parameterized
  size/`is_free` round-trip tests, and payload-alignment tests against fake
  headers at multiple aligned addresses (34 GoogleTest cases, all passing).
- `docs/architecture.md`, `docs/memory-model.md`, `docs/design-decisions.md`:
  layered architecture overview, block layout explanation, and the first two
  design-decision log entries (`mmap` vs. `sbrk`, bit-packed `is_free`).

### Added — Phase 2: Basic bump allocator
- `os_memory` (`include/os_memory.hpp`/`src/os_memory.cpp`): `mmap`/`munmap`
  wrapper for acquiring/releasing memory from the OS.
- Real `my_malloc`/`my_free` backed by a single growing arena, bumping
  forward on allocation with no reuse of freed memory yet.

### Added — Phase 3: Free list
- Intrusive, doubly-linked free list (`include/free_list.hpp`/
  `src/free_list.cpp`) reusing freed blocks via `BlockHeader::FreeListLinks`
  instead of only bumping forward; first-fit search.

### Added — Phase 4: Splitting and coalescing
- Block splitting: an oversized free block found by the free-list search is
  carved down to the requested size, with the remainder re-inserted as a
  new free block.
- Boundary-tag (`BlockFooter`) based coalescing: adjacent free blocks are
  merged on `my_free()` to fight fragmentation.

### Added — Phase 5: `calloc`/`realloc`
- `my_calloc`: overflow-checked, always-zeroed allocation.
- `my_realloc`: in-place growth (absorbing a free right neighbor via
  Phase 4's coalescing machinery) or shrink (via splitting) where possible,
  allocate+copy+free fallback otherwise.

### Added — Phase 6: Memory pools / size classes
- `Pool` (`include/memory_pool.hpp`/`src/memory_pool.cpp`): fixed-size slot
  pools for 9 size classes, O(1) allocation/deallocation with zero search,
  splitting, or coalescing, as a second allocation path for small, common
  sizes alongside the general free-list allocator.
- Pointer-to-pool routing via a validated speculative tag
  (`PoolSlotHeader::owning_pool` checked against `is_known_pool()`) rather
  than an arena range check -- see design-decisions.md entry 5.

### Added — Phase 7: Concurrency and performance benchmarking (final phase)
- Thread safety across `my_malloc`/`my_free`/`my_calloc`/`my_realloc`,
  built as three separately selectable, compile-time-chosen stages
  (`ALLOCATOR_CONCURRENCY_STAGE`): a single global `std::mutex` (Stage 1),
  per-size-class locks plus one general-path lock (Stage 2), and
  `thread_local` per-thread slot caching with batched refill/flush on top
  of Stage 2's locking (Stage 3, the default) -- see design-decisions.md
  entries 6-8.
- `Pool::allocate_batch`/`Pool::deallocate_batch`: O(1) batch pop/splice
  operations on the pool's free-slot chain, used by Stage 3 to amortize
  lock acquisition across many slots at once.
- `tests/test_thread_safety.cpp`: multithreaded mixed alloc/free-cycle
  test, a dedicated cross-thread-free test (allocate on one thread, free on
  another), and a single-size-class contention test, all verified with
  zero data races under ThreadSanitizer; the full previously-existing test
  suite is unmodified and still passes.
- `benchmarks/benchmark_allocator.cpp`: hand-rolled `std::chrono` harness
  measuring single-threaded malloc/free latency (mean and p99) against
  system `malloc`, and multithreaded throughput at 1/2/4/8 threads for
  every concurrency stage.
- `benchmarks/benchmark_fragmentation.cpp`: simplified fragmentation-
  awareness indicator reporting peak RSS alongside total live bytes.
- `scripts/run_benchmarks.sh`: reproducible Release-mode build-and-run of
  every benchmark executable.
- `docs/benchmarks.md`: populated with real measured results from this
  machine, including honest discussion of surprising findings (Stage 3 not
  clearly beating Stage 2 at higher thread counts on this hardware, and
  this allocator trailing glibc on single-threaded malloc latency).
- Fixed a genuine (if practically benign) undefined-behavior issue in
  `BlockHeader`: `size_and_flags_` was read before its first write on
  construction; given a default member initializer.

### Project status: all 7 phases complete
This project's plan was trimmed from an original 13 phases to 7 partway
through (see design-decisions.md entry 4). With Phase 7 done, the project
has reached the full scope of that trimmed plan: a working, tested,
thread-safe, benchmarked general-purpose allocator with pooled and
general allocation paths, going from raw `mmap` calls to concurrent
throughput measured and documented end to end. No further phases are
planned; remaining known limitations (caller-requested alignment, a
formal CI workflow, and others) are listed in `README.md`'s "Cut from the
original 13-phase plan" section rather than silently dropped.
