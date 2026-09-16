#pragma once

#include <cstddef>

// ---------------------------------------------------------------------------
// allocator
//
// Public allocator API. This is the only header application code is meant
// to include directly.
//
// Phase 4 status: my_malloc() first tries to satisfy a request by reusing a
// block from the free list (see free_list.hpp). If the found block is
// larger than needed by at least kMinBlockSize, it's split: shrunk to the
// requested size, with the leftover carved into a new free block and
// reinserted into the free list. If the leftover would be too small to
// stand on its own as a block, the whole free block is handed over
// instead. If no free block fits at all, my_malloc() falls back to
// bump-pointer allocation over mmap-backed arenas (see os_memory.hpp),
// requesting a new arena from the OS when the current one runs out of
// room. my_free() marks a block free, coalesces it with any free physical
// neighbors (within the same arena -- see the arena-boundary-safety
// helpers in allocator.cpp), and inserts the resulting block into the
// free list, making it available for reuse. See src/allocator.cpp and
// docs/memory-model.md for the full rationale and worked examples.
// ---------------------------------------------------------------------------

namespace allocator {

// Allocates at least `size` bytes and returns a pointer to the start of the
// usable region, suitably aligned for any object type
// (alignof(std::max_align_t)). Returns nullptr if the underlying OS memory
// request fails. Prefers reusing a free-list block over acquiring new OS
// memory; see the file-level comment above for what "reuse" does and does
// not do yet.
[[nodiscard]] void* my_malloc(std::size_t size) noexcept;

// Allocates storage for `count` objects of `size` bytes each and
// zero-initializes the entire region, matching the standard calloc()
// contract. Detects count * size multiplication overflow before
// allocating anything and returns nullptr in that case -- this is real,
// defined failure behavior, not undefined behavior; see src/allocator.cpp
// for the overflow-check approach and why it matters (a naive
// malloc(count * size) that silently wraps around is a classic real-world
// calloc vulnerability class). Also returns nullptr if the underlying
// allocation itself fails.
[[nodiscard]] void* my_calloc(std::size_t count, std::size_t size) noexcept;

// Marks the block backing `ptr` (as returned by a prior my_malloc() call)
// free, coalesces it with any free physical neighbors within the same
// arena, and inserts the resulting block into the free list for future
// reuse. `ptr` must have been returned by my_malloc() and not already
// freed -- passing any other pointer, or double-freeing, is undefined
// behavior (there is no double-free detection yet). A nullptr `ptr` is a
// no-op, matching the standard free() convention.
void my_free(void* ptr) noexcept;

// Releases every OS arena acquired via my_malloc() back to the OS. This
// exists for tests and examples that want to avoid leaking OS memory across
// repeated runs within the same process; a real embedder of this allocator
// has no reason to call it under normal operation before process exit.
// Any pointers previously returned by my_malloc() are invalidated by this
// call.
void allocator_shutdown() noexcept;

// ---------------------------------------------------------------------------
// Test-only hooks. Production code and examples (basic_allocation.cpp)
// never call these -- the default arena size always applies unless a test
// explicitly overrides it. They exist so arena-rollover behavior can be
// exercised deterministically with a small arena instead of only inferred
// from allocating megabytes of throwaway data.
// ---------------------------------------------------------------------------

// Overrides the arena size my_malloc() requests for future arena
// acquisitions, until reset_arena_size_for_testing() is called.
void set_arena_size_for_testing(std::size_t size) noexcept;

// Restores the default arena size (1 MiB), undoing any prior
// set_arena_size_for_testing() call. Tests that override the arena size
// must call this during teardown so later tests aren't affected.
void reset_arena_size_for_testing() noexcept;

// Number of arenas currently tracked (i.e. that allocator_shutdown() would
// release). Lets tests assert directly that a rollover created a new arena.
[[nodiscard]] std::size_t arena_count_for_testing() noexcept;

// Returns true if the block backing `ptr` (as returned by a prior
// my_malloc() call) has a free right/left physical neighbor within the
// same arena. `ptr`'s own block may be allocated or free -- these only
// inspect the neighbor. Exists to test the arena-boundary-safe neighbor
// detection Phase 4's coalescing is built on (see the arena-boundary-safety
// helpers in allocator.cpp) independently of the coalescing logic itself,
// so the two can be verified separately.
[[nodiscard]] bool right_neighbor_is_free_for_testing(void* ptr) noexcept;
[[nodiscard]] bool left_neighbor_is_free_for_testing(void* ptr) noexcept;

// Number of blocks currently linked into the internal free list. Lets
// coalescing tests assert precisely that a merge left exactly one entry
// behind, not two or three -- exactly the shape a double-insert or
// forgotten-remove bug would fail to satisfy.
[[nodiscard]] std::size_t free_list_size_for_testing() noexcept;

} // namespace allocator
