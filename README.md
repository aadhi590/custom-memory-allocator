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

**Phase 0-4 complete** — block metadata, a real `mmap`-backed allocator with
free-list reuse, and block splitting/coalescing.

`my_malloc`/`my_free` (`include/allocator.hpp`) are backed by `os_memory`'s
`mmap`/`munmap` wrapper, a doubly-linked free list (`include/free_list.hpp`)
searched first-fit, and `include/block.hpp`'s `BlockHeader`/`BlockFooter`
boundary-tag pair. `my_malloc` reuses a free block when one fits, splitting
it if the leftover is large enough to stand on its own; `my_free` marks a
block free and coalesces it with any free physical neighbors within the same
arena before returning it to the free list. See
[docs/memory-model.md](docs/memory-model.md) for the full block-layout
explanation plus worked examples of splitting and coalescing, and
[docs/design-decisions.md](docs/design-decisions.md) for the reasoning behind
specific choices. Reallocation (`my_realloc`) and multiple concurrent memory
pools don't exist yet — those start at Phase 5/6.

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
- [ ] **Phase 5** — Alignment: support caller-requested alignments beyond the
      default.
- [ ] **Phase 6** — Memory pools: multiple mmap-backed arenas, pool growth
      and management.
- [ ] **Phase 7** — Realloc: `my_realloc` with in-place growth/shrink where
      possible.
- [ ] **Phase 8** — Thread safety: locking strategy, then a per-thread cache
      to reduce contention.
- [ ] **Phase 9** — Instrumentation: allocation statistics, benchmarking
      harness, stress tests.
- [ ] **Phase 10** — Hardening: corruption detection, fuzzing, sanitizer
      coverage.
- [ ] **Phase 11** — CI: GitHub Actions workflows (build, test, sanitizers).
- [ ] **Phase 12** — Examples and API polish: example programs, documentation
      pass.

## License

MIT — see [LICENSE](LICENSE).
