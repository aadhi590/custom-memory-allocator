#pragma once

#include <cstddef>
#include <vector>

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
//
// Pooled blocks intentionally do NOT use block.hpp's BlockHeader. A
// BlockHeader exists to carry a size (needed because general-path blocks
// vary in size and must self-describe) plus room for boundary-tag
// coalescing. Neither applies to a pool slot: its size is implied by
// which pool it came from, and pool slots never coalesce (there's no
// concept of a "neighboring" slot worth merging with -- every slot in a
// pool is interchangeable). PoolSlotHeader below is the deliberately
// lighter-weight alternative -- see its own comment for the full
// trade-off, including how my_free() (allocator.cpp) still identifies
// "which pool does this pointer belong to" without a size field to lean
// on.
// ---------------------------------------------------------------------------

namespace allocator {

class Pool;

// A pool slot's header-sized region, immediately before the payload a
// caller receives from Pool::allocate() -- reinterpreted for two
// different purposes depending on whether the slot is currently free or
// allocated, the same overlay concept as block.hpp's
// FreeListLinks/payload distinction:
//   - Free: `next_free` chains this slot into its pool's internal
//     free-slot list. Only a single forward link is needed (unlike the
//     general free list's FreeListLinks, which needs `prev` too for O(1)
//     removal from the middle of the list during coalescing) -- a pool's
//     free-slot list only ever needs O(1) push/pop at the head, since
//     pool slots never coalesce and are never removed from the middle.
//   - Allocated: `owning_pool` records which Pool this slot belongs to.
//     This is how my_free() identifies "this pointer came from pool X"
//     and routes deallocation correctly -- see the routing entry in
//     docs/design-decisions.md for the full trade-off against the
//     alternative (a per-pool arena address-range check) and why this
//     was chosen instead.
// Forced to alignof(std::max_align_t), exactly like BlockHeader/
// BlockFooter, so that consecutive fixed-size slots in an arena keep
// every payload aligned.
union alignas(alignof(std::max_align_t)) PoolSlotHeader {
    PoolSlotHeader* next_free;
    Pool* owning_pool;
};

static_assert(sizeof(PoolSlotHeader) % alignof(std::max_align_t) == 0,
              "PoolSlotHeader size must be a multiple of alignof(std::max_align_t) so "
              "consecutive slots' payloads stay aligned");

// Manages fixed-size slot allocation for ONE size class. Slots are carved
// from mmap-backed arenas (via os_memory.hpp, reused unchanged from
// Phase 2 -- Pool never calls mmap/munmap directly) and chained into an
// internal singly-linked free-slot list immediately on arena acquisition.
class Pool {
public:
    explicit Pool(std::size_t slot_payload_size) noexcept;

    // Returns a pointer to a slot_payload_size()-byte usable region, or
    // nullptr if this pool needed to expand (request a new arena from the
    // OS) and that request failed. O(1) on the common path: pops the
    // free-slot list head. Only falls back to requesting a new arena
    // (see expand()) when the free-slot list is empty.
    [[nodiscard]] void* allocate() noexcept;

    // Returns a slot (as returned by a prior allocate() call on this
    // exact pool) to the free-slot list. O(1): pushes onto the list head.
    // The caller (my_free()) is responsible for having already
    // determined `ptr` really belongs to this pool.
    void deallocate(void* ptr) noexcept;

    [[nodiscard]] std::size_t slot_payload_size() const noexcept { return slot_payload_size_; }

    // Releases every arena this pool has acquired back to the OS and
    // resets the free-slot list. Same test/example cleanup role as
    // allocator_shutdown() for the general path -- see allocator.cpp.
    void release_all_arenas() noexcept;

    // Number of arenas this pool has acquired so far. Test-only: lets
    // pool-expansion tests assert directly that a second arena was
    // created, mirroring arena_count_for_testing() for the general path.
    [[nodiscard]] std::size_t arena_count_for_testing() const noexcept { return arenas_.size(); }

private:
    struct Arena {
        void* base;
        std::size_t size;
    };

    // Requests a new arena sized for kSlotsPerArena slots (or however
    // many whole slots the OS's page-rounded region actually holds, if
    // more), chaining all of its slots into the free-slot list. Returns
    // false if the underlying OS memory request fails.
    bool expand() noexcept;

    std::size_t slot_payload_size_;
    std::size_t slot_total_size_; // sizeof(PoolSlotHeader) + slot_payload_size_
    PoolSlotHeader* free_head_ = nullptr;
    std::vector<Arena> arenas_;
};

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
