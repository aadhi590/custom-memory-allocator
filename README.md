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

**Phase 0/1 complete** — project scaffolding and block metadata layout.

`include/block.hpp` defines `BlockHeader`, the per-block metadata struct that
will prefix every block the allocator manages: a bit-packed size/`is_free`
field, an overlay mechanism for free-list `next`/`prev` pointers vs. user
payload, and a `BlockFooter` boundary-tag type for future O(1) coalescing.
See [docs/memory-model.md](docs/memory-model.md) for the full explanation and
[docs/design-decisions.md](docs/design-decisions.md) for the reasoning behind
specific choices. No allocation logic (`my_malloc`/`my_free`, `mmap` calls,
free lists) exists yet — that starts at Phase 2.

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
include/     public headers (block.hpp is the only one with real content so far)
src/         implementation files (currently empty stubs)
tests/       GoogleTest test suites (test_block_layout.cpp is the only real one so far)
benchmarks/  throughput/fragmentation benchmarks (Phase 9+)
examples/    example programs using the allocator (Phase 2+)
docs/        architecture, memory model, design decisions, benchmark results
scripts/     developer tooling (Phase 9+)
```

## Roadmap

- [x] **Phase 0** — Project scaffolding: repo layout, CMake + GoogleTest
      build, licensing, tooling config.
- [x] **Phase 1** — Block metadata: `BlockHeader` layout, bit-packed
      size/`is_free`, free-list pointer overlay, boundary-tag footer concept.
- [ ] **Phase 2** — Basic bump allocator: `os_memory` mmap wrapper, real
      `my_malloc`/`my_free` backed by a single growing region.
- [ ] **Phase 3** — Free list: reuse freed blocks via an intrusive free list
      instead of only bumping forward.
- [ ] **Phase 4** — Splitting and coalescing: carve oversized free blocks
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
