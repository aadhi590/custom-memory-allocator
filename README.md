# custom-memory-allocator

A general-purpose, from-scratch heap memory allocator written in C++17 for
Linux (x86-64). It implements the core mechanics of a real allocator —
mmap-backed memory pools, block splitting and coalescing, size-class free
lists, and eventually per-thread caching — as a portfolio project targeting
systems/embedded C++ roles.

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

**Phase 0-5 complete** — block metadata, a real `mmap`-backed allocator with
free-list reuse, block splitting/coalescing, and `calloc`/`realloc`.

`my_malloc`/`my_free` (`include/allocator.hpp`) are backed by `os_memory`'s
`mmap`/`munmap` wrapper, a doubly-linked free list (`include/free_list.hpp`)
searched first-fit, and `include/block.hpp`'s `BlockHeader`/`BlockFooter`
boundary-tag pair. `my_malloc` reuses a free block when one fits, splitting
it if the leftover is large enough to stand on its own; `my_free` marks a
block free and coalesces it with any free physical neighbors within the same
arena before returning it to the free list. `my_calloc` layers
overflow-checked, always-unconditional zero-initialization on top of
`my_malloc`; `my_realloc` reuses the same splitting/coalescing machinery to
grow a block in place when its free right neighbor is large enough, falling
back to allocate+copy+free otherwise. See
[docs/memory-model.md](docs/memory-model.md) for the full block-layout
explanation plus worked examples of splitting and coalescing, and
[docs/design-decisions.md](docs/design-decisions.md) for the reasoning behind
specific choices. Caller-requested alignment beyond the default, multiple
concurrent memory pools, and thread safety don't exist yet.

## Build instructions

Requires a C++17 compiler, CMake 3.16+, and network access on first configure
(to fetch GoogleTest via `FetchContent`). Linux x86-64 only.

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

Pass `-DCMAKE_BUILD_TYPE=Release` to `cmake -B build` for an optimized build;
Debug is the default.

## Repository layout

```
include/     public headers (block.hpp, allocator.hpp, free_list.hpp, os_memory.hpp have real content)
src/         implementation files (allocator.cpp, free_list.cpp, os_memory.cpp have real content)
tests/       GoogleTest test suites
benchmarks/  throughput/fragmentation benchmarks (Phase 9+)
examples/    example programs using the allocator (basic_allocation.cpp)
docs/        architecture, memory model, design decisions, benchmark results
scripts/     developer tooling (Phase 9+)
```

## Roadmap

**The project plan was trimmed from an original 13 phases to 7, partway
through, for time.** Phases 0-5 below are unchanged from the original plan.
Phase 6 is this project's current phase. Phase 7 is a merged final phase
covering what the original plan spread across separate concurrency,
benchmarking, hardening, and polish phases. See the "Scope reduction:
13 phases to 7" entry in [docs/design-decisions.md](docs/design-decisions.md)
for the full rationale and exactly what was cut.

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
- [ ] **Phase 6** — Memory pools / size classes: fixed-size slot pools for
      small, common allocation sizes as a second, O(1) allocation path
      alongside the general free-list allocator (which continues to handle
      everything above the largest size class).
- [ ] **Phase 7** — Concurrency and performance benchmarking (merged final
      phase): a locking progression for thread safety, thread-local caching
      to reduce contention, and benchmarking the allocator's performance.

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
