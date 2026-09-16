#pragma once

#include <cstddef>

// ---------------------------------------------------------------------------
// allocator
//
// Public allocator API. This is the only header application code is meant
// to include directly.
//
// Phase 2 status (Level 1 baseline): a bump-pointer allocator over
// mmap-backed arenas (see os_memory.hpp). my_malloc() hands out memory by
// advancing a pointer through the current arena and requesting a new arena
// from the OS when the current one runs out of room. my_free() is an
// intentional no-op at this phase -- freed memory is never reclaimed or
// reused until Phase 3 introduces a free list. See src/allocator.cpp for
// the full rationale.
// ---------------------------------------------------------------------------

namespace allocator {

// Allocates at least `size` bytes and returns a pointer to the start of the
// usable region, suitably aligned for any object type
// (alignof(std::max_align_t)). Returns nullptr if the underlying OS memory
// request fails.
[[nodiscard]] void* my_malloc(std::size_t size) noexcept;

// Phase 2 (Level 1) behavior: intentional no-op. See src/allocator.cpp for
// why -- this is not a placeholder that was forgotten, it's the documented
// behavior of the bump-pointer baseline. Real deallocation arrives in
// Phase 3.
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

} // namespace allocator
