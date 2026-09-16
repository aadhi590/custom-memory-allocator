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
