#pragma once

#include <cstddef>

// ---------------------------------------------------------------------------
// memory_pool
//
// Phase 6 adds a SECOND allocation path alongside the general free-list
// allocator (block.hpp/free_list.hpp/allocator.cpp, Phases 3-5), not a
// replacement for it. Small, common allocation sizes are routed to
// fixed-size "pools": allocation and deallocation within a pool are O(1)
// with zero search, zero splitting, and zero coalescing -- the general
// path's more expensive machinery exists to handle irregular, arbitrary
// sizes, which pools intentionally don't try to serve. Anything larger
// than the biggest size class continues through the general path,
// completely unchanged from Phase 5.
// ---------------------------------------------------------------------------

namespace allocator {

// Size classes this allocator routes small allocations to, in ascending
// order. Anything <= kSizeClasses[kNumSizeClasses - 1] (4096) rounds UP
// to the smallest class that fits; anything larger uses the general
// free-list/arena path instead.
inline constexpr std::size_t kNumSizeClasses = 9;
inline constexpr std::size_t kSizeClasses[kNumSizeClasses] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

// Sentinel returned by size_class_for() meaning "no size class fits --
// use the general free-list/arena path instead." Never a real size class
// value (all real size classes are small positive numbers; this is the
// largest possible std::size_t).
inline constexpr std::size_t kNoSizeClass = static_cast<std::size_t>(-1);

// Rounds `requested_payload_size` up to the smallest size class that can
// hold it (e.g. 100 -> 128), or returns kNoSizeClass if it exceeds the
// largest size class (4096 bytes). A pure function with no allocator
// state, deliberately kept small and directly unit-testable on its own --
// see tests/test_memory_pool.cpp for exhaustive boundary coverage.
[[nodiscard]] std::size_t size_class_for(std::size_t requested_payload_size) noexcept;

} // namespace allocator
