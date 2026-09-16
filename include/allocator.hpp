#pragma once

#include <cstddef>

// ---------------------------------------------------------------------------
// allocator
//
// Public allocator API. This is the only header application code is meant
// to include directly.
//
// Phase 6 status: my_malloc() first checks size_class_for(size)
// (memory_pool.hpp). A real size class (<= 4096 bytes) delegates to that
// class's fixed-size Pool, an O(1) allocation path with zero search, zero
// splitting, and zero coalescing. Anything too large for any size class
// falls through to the general path unchanged from Phase 3-5: reusing a
// block from the free list (see free_list.hpp), splitting it if the
// leftover is large enough to stand on its own, or falling back to
// bump-pointer allocation over mmap-backed arenas (see os_memory.hpp).
// my_free() and my_realloc() both first determine whether `ptr` is a
// pooled or general-path pointer (see the pointer-to-pool routing entry
// in docs/design-decisions.md) and dispatch accordingly: pooled
// deallocation/reuse is O(1) with no coalescing (pool slots have no
// adjacent free-space concept), while general-path pointers get Phase 4's
// coalescing and Phase 5's grow-in-place/copy-fallback realloc logic,
// unchanged. my_calloc() layers overflow-checked zero-initialization on
// top of my_malloc() and works identically regardless of which path
// served the allocation. See src/allocator.cpp, docs/memory-model.md, and
// docs/design-decisions.md for the full rationale and worked examples.
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

// Frees the allocation backing `ptr` (as returned by a prior my_malloc()/
// my_calloc()/my_realloc() call), routing to the correct deallocation
// path automatically depending on whether `ptr` is pooled (O(1), no
// coalescing) or general-path (coalesces with any free physical
// neighbors within the same arena before returning to the free list).
// `ptr` must have been returned by this allocator and not already freed
// -- passing any other pointer, or double-freeing, is undefined behavior
// (there is no double-free detection yet). A nullptr `ptr` is a no-op,
// matching the standard free() convention.
void my_free(void* ptr) noexcept;

// Resizes the allocation backing `ptr` to at least `new_size` bytes,
// following the standard realloc() contract:
//   - my_realloc(nullptr, new_size) behaves exactly like my_malloc(new_size).
//   - If new_size already fits within ptr's current usable size, ptr is
//     returned unchanged (no copy, no reallocation). This includes
//     my_realloc(ptr, 0), a deliberate choice -- see src/allocator.cpp for
//     why, and how it's consistent with my_malloc(0)'s existing behavior.
//     This holds for both pooled and general-path pointers.
//   - If growing a GENERAL-PATH pointer, and ptr's free right physical
//     neighbor (same arena) exists and is large enough, the block grows
//     in place by absorbing it (splitting off any excess back to the free
//     list) and the SAME pointer is returned -- no data is copied, since
//     the payload never moved.
//   - If growing a POOLED pointer, growing in place never happens --
//     pool slots are fixed-size with no adjacent free-space concept the
//     way general-path blocks have, so any growth beyond the current
//     slot's capacity always falls through to the allocate+copy+free path
//     below, even if the new size would fit in the same or a
//     neighboring size class.
//   - Otherwise (general-path growth with no usable neighbor, or any
//     pooled growth), a new block is allocated via my_malloc(), the
//     lesser of the old and new sizes is copied over, the old block is
//     freed via my_free(), and the new pointer is returned.
//   - If growing requires a new allocation and that allocation fails,
//     `ptr` and its contents are left completely untouched and nullptr is
//     returned -- a failed realloc() must never leak or corrupt the
//     original allocation.
[[nodiscard]] void* my_realloc(void* ptr, std::size_t new_size) noexcept;

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

// The current payload size (get_size()) of the block backing `ptr`, as
// returned by a prior my_malloc()/my_calloc()/my_realloc() call. Lets
// tests verify things like "my_realloc()'s grow-in-place split the excess
// back to the free list, rather than silently handing over an entire
// absorbed neighbor" by checking the returned block's actual reported
// size, not just its address or contents.
[[nodiscard]] std::size_t block_payload_size_for_testing(void* ptr) noexcept;

} // namespace allocator
