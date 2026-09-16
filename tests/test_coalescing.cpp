#include "allocator.hpp"

#include <cstddef>
#include <cstring>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "block.hpp"

namespace {

class CoalescingTest : public ::testing::Test {
protected:
    void TearDown() override {
        allocator::allocator_shutdown();
        allocator::reset_arena_size_for_testing();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Arena-boundary safety
//
// A block's physical neighbor only exists if it lies within the SAME
// arena's carved region. Reading a footer or header that happens to sit
// just outside an arena's mmap'd region -- because this block is the
// first or last block carved into its arena -- would read unmapped or
// unrelated memory: a real bug class, not a hypothetical one. These tests
// exercise exactly that edge (first block of an arena, last block carved
// into an arena) via the arena-boundary-safety primitives
// (right_neighbor_is_free_for_testing()/left_neighbor_is_free_for_testing(),
// see allocator.cpp's arena_owning()/right_neighbor_if_free()/
// left_neighbor_if_free()) that Phase 4's coalescing is built on.
//
// Coalescing itself isn't wired into my_free() until later in this same
// phase (see the FreesRightNeighbor/FreesLeftNeighbor/etc. tests further
// down, once that lands) -- these tests verify the underlying
// boundary-safe neighbor detection is correct and does not read outside
// the arena's mapped region *before* anything depends on it. This is
// deliberately run under AddressSanitizer (see the project's ASan build,
// -fsanitize=address,undefined): a bounds-check bug here would either
// crash (if the adjacent memory happens to be unmapped) or silently read
// garbage (if it happens to be mapped), and ASan is what turns the latter,
// scarier case into a loud, attributable failure instead of a passing test
// that got lucky with heap layout.
TEST_F(CoalescingTest, FirstBlockInArenaHasNoLeftNeighbor) {
    const auto page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    allocator::set_arena_size_for_testing(page_size);

    void* first = allocator::my_malloc(64);
    ASSERT_NE(first, nullptr);

    // The very first block carved into its arena has nothing before it --
    // must report no left neighbor, and must not read before arena.base to
    // find out.
    EXPECT_FALSE(allocator::left_neighbor_is_free_for_testing(first));
}

TEST_F(CoalescingTest, LastBlockCarvedInArenaHasNoRightNeighborYet) {
    const auto page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    allocator::set_arena_size_for_testing(page_size);

    constexpr std::size_t kPayload = 64;
    const std::size_t needed_per_alloc = sizeof(allocator::BlockHeader) + kPayload + sizeof(allocator::BlockFooter);
    const std::size_t capacity_per_arena = page_size / needed_per_alloc;
    ASSERT_GT(capacity_per_arena, 0u);

    std::vector<void*> ptrs;
    ptrs.reserve(capacity_per_arena);
    for (std::size_t i = 0; i < capacity_per_arena; ++i) {
        void* ptr = allocator::my_malloc(kPayload);
        ASSERT_NE(ptr, nullptr);
        ptrs.push_back(ptr);
    }

    // All of these allocations must have landed in the same, first arena --
    // otherwise the test isn't exercising the boundary it claims to.
    ASSERT_EQ(allocator::arena_count_for_testing(), 1u);

    // The last block carved into the arena has nothing after it yet --
    // must report no right neighbor, and must not read past
    // arena.base + arena.used (even though more *mapped* space may still
    // remain within arena.size) to find out.
    EXPECT_FALSE(allocator::right_neighbor_is_free_for_testing(ptrs.back()));

    // One more allocation must roll over into a second arena, confirming
    // the previous block really was the last one carved into arena 1.
    void* rolled_over = allocator::my_malloc(kPayload);
    ASSERT_NE(rolled_over, nullptr);
    EXPECT_EQ(allocator::arena_count_for_testing(), 2u);

    // Re-check after the rollover: arena 1's last block still correctly
    // reports no right neighbor within arena 1, even though a block now
    // exists (in a different arena) at a higher address.
    EXPECT_FALSE(allocator::right_neighbor_is_free_for_testing(ptrs.back()));
}

TEST_F(CoalescingTest, MiddleBlockNeighborsAreDetectedButNotFreeYet) {
    const auto page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    allocator::set_arena_size_for_testing(page_size);

    constexpr std::size_t kPayload = 64;
    void* a = allocator::my_malloc(kPayload);
    void* b = allocator::my_malloc(kPayload);
    void* c = allocator::my_malloc(kPayload);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    // b sits between two real, currently-allocated neighbors -- neighbor
    // detection must not crash, and must correctly report "not free"
    // (they exist, they're just allocated) rather than "no neighbor".
    EXPECT_FALSE(allocator::left_neighbor_is_free_for_testing(b));
    EXPECT_FALSE(allocator::right_neighbor_is_free_for_testing(b));
}
