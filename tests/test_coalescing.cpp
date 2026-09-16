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

using allocator::BlockFooter;
using allocator::BlockHeader;

BlockHeader* header_of(void* payload) {
    return reinterpret_cast<BlockHeader*>(static_cast<std::byte*>(payload) - sizeof(BlockHeader));
}

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

// ---------------------------------------------------------------------------
// Coalescing behavior, once wired into my_free()
// ---------------------------------------------------------------------------

// Case 1: freeing a block whose right neighbor is already free must merge
// them into a single block, and the old right-neighbor entry must not
// remain separately reachable in the free list.
TEST_F(CoalescingTest, FreeingBlockWithFreeRightNeighborMergesThem) {
    void* a = allocator::my_malloc(64);
    void* b = allocator::my_malloc(64);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    allocator::my_free(b); // b free; a still allocated -- no merge yet
    ASSERT_EQ(allocator::free_list_size_for_testing(), 1u);

    allocator::my_free(a); // a's right neighbor (b) is free -> merge

    EXPECT_EQ(allocator::free_list_size_for_testing(), 1u); // one entry, not two

    BlockHeader* merged = header_of(a);
    EXPECT_TRUE(merged->is_free());
    constexpr std::size_t kExpectedCombined = 64 + sizeof(BlockFooter) + sizeof(BlockHeader) + 64;
    EXPECT_EQ(merged->get_size(), kExpectedCombined);
    EXPECT_EQ(merged->footer()->size, kExpectedCombined);
}

// Case 2: mirror of case 1 -- freeing a block whose left neighbor is
// already free must merge them, with the left neighbor's header surviving
// as the merged block's header.
TEST_F(CoalescingTest, FreeingBlockWithFreeLeftNeighborMergesThem) {
    void* a = allocator::my_malloc(64);
    void* b = allocator::my_malloc(64);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    allocator::my_free(a); // a free; b still allocated -- no merge yet
    ASSERT_EQ(allocator::free_list_size_for_testing(), 1u);

    allocator::my_free(b); // b's left neighbor (a) is free -> merge

    EXPECT_EQ(allocator::free_list_size_for_testing(), 1u); // one entry, not two

    BlockHeader* merged = header_of(a);
    EXPECT_TRUE(merged->is_free());
    constexpr std::size_t kExpectedCombined = 64 + sizeof(BlockFooter) + sizeof(BlockHeader) + 64;
    EXPECT_EQ(merged->get_size(), kExpectedCombined);
    EXPECT_EQ(merged->footer()->size, kExpectedCombined);
}

// Case 3: both neighbors free -- the trickiest case. Freeing the middle
// block of three must merge all three into one, with exactly one free-list
// entry for the whole combined region afterward.
TEST_F(CoalescingTest, FreeingBlockWithBothNeighborsFreeMergesAllThree) {
    void* a = allocator::my_malloc(64);
    void* b = allocator::my_malloc(64);
    void* c = allocator::my_malloc(64);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    allocator::my_free(a);
    allocator::my_free(c);
    ASSERT_EQ(allocator::free_list_size_for_testing(), 2u); // a, c separate -- b still allocated between them

    allocator::my_free(b); // both neighbors free -> merge all three

    EXPECT_EQ(allocator::free_list_size_for_testing(), 1u);

    BlockHeader* merged = header_of(a);
    EXPECT_TRUE(merged->is_free());
    constexpr std::size_t kExpectedCombined = 3 * 64 + 2 * (sizeof(BlockFooter) + sizeof(BlockHeader));
    EXPECT_EQ(merged->get_size(), kExpectedCombined);
    EXPECT_EQ(merged->footer()->size, kExpectedCombined);
}

// Case 4: regression check against over-eager coalescing -- if neither
// neighbor is free, no merge may occur at all.
TEST_F(CoalescingTest, FreeingIsolatedBlockDoesNotMergeWithAllocatedNeighbors) {
    void* a = allocator::my_malloc(64);
    void* b = allocator::my_malloc(64);
    void* c = allocator::my_malloc(64);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    allocator::my_free(b); // a and c remain allocated

    EXPECT_EQ(allocator::free_list_size_for_testing(), 1u);

    BlockHeader* b_header = header_of(b);
    EXPECT_TRUE(b_header->is_free());
    EXPECT_EQ(b_header->get_size(), 64u); // unchanged -- no merge occurred

    // a and c must remain independently allocated and untouched by
    // whatever bookkeeping ran around freeing b.
    std::memset(a, 0xAA, 64);
    std::memset(c, 0xCC, 64);
    const auto* a_bytes = static_cast<const unsigned char*>(a);
    const auto* c_bytes = static_cast<const unsigned char*>(c);
    for (std::size_t i = 0; i < 64; ++i) {
        ASSERT_EQ(a_bytes[i], 0xAA) << "a byte " << i;
        ASSERT_EQ(c_bytes[i], 0xCC) << "c byte " << i;
    }
}

// Case 5: the arena-boundary primitives are tested directly earlier in
// this file (FirstBlockInArenaHasNoLeftNeighbor /
// LastBlockCarvedInArenaHasNoRightNeighborYet). This is the same edge
// exercised through the real my_free() coalescing path: freeing the very
// first and very last blocks carved into a small arena must not attempt
// to merge with neighbors that don't exist, and must not read outside the
// arena while checking.
TEST_F(CoalescingTest, FreeingFirstAndLastBlockInArenaDoesNotMergeOutsideIt) {
    const auto page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    allocator::set_arena_size_for_testing(page_size);

    constexpr std::size_t kPayload = 64;
    const std::size_t needed_per_alloc = sizeof(BlockHeader) + kPayload + sizeof(BlockFooter);
    const std::size_t capacity_per_arena = page_size / needed_per_alloc;
    ASSERT_GE(capacity_per_arena, 2u);

    std::vector<void*> ptrs;
    ptrs.reserve(capacity_per_arena);
    for (std::size_t i = 0; i < capacity_per_arena; ++i) {
        void* ptr = allocator::my_malloc(kPayload);
        ASSERT_NE(ptr, nullptr);
        ptrs.push_back(ptr);
    }
    ASSERT_EQ(allocator::arena_count_for_testing(), 1u);

    allocator::my_free(ptrs.front()); // no left neighbor to merge with
    allocator::my_free(ptrs.back());  // no right neighbor to merge with yet

    // The middle blocks are all still allocated, so these two frees must
    // remain two independent, uncoalesced free-list entries.
    EXPECT_EQ(allocator::free_list_size_for_testing(), 2u);
}

// Case 6: proves coalescing actually enables reuse that neither original
// half could satisfy alone -- the core reason coalescing exists.
TEST_F(CoalescingTest, CoalescedBlockSatisfiesRequestNeitherHalfCouldAlone) {
    void* a = allocator::my_malloc(64);
    void* b = allocator::my_malloc(64);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    allocator::my_free(a);
    allocator::my_free(b); // adjacent frees -> coalesce into one larger block

    // align_up(100) == 112, which exceeds either original 64-byte half on
    // its own but fits the combined 64+16+16+64=160-byte block.
    void* reused = allocator::my_malloc(100);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused, a); // combined block's surviving header is a's

    BlockHeader* reused_header = header_of(reused);
    EXPECT_EQ(reused_header->get_size(), 112u); // shrunk via splitting after coalescing enabled the fit

    std::memset(reused, 0x77, 100);
    const auto* bytes = static_cast<const unsigned char*>(reused);
    for (std::size_t i = 0; i < 100; ++i) {
        ASSERT_EQ(bytes[i], 0x77) << "byte " << i;
    }
}

// Double-free-adjacent regression check: after a three-way merge, neither
// absorbed neighbor may still be separately reachable through the free
// list (a double-insert or forgotten-remove bug would leave a stale entry
// behind that this test would find instead of correctly reporting the
// list empty).
TEST_F(CoalescingTest, AbsorbedNeighborsAreNeverReinsertedOrDoubleAllocated) {
    void* a = allocator::my_malloc(64);
    void* b = allocator::my_malloc(64);
    void* c = allocator::my_malloc(64);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    allocator::my_free(a);
    allocator::my_free(c);
    allocator::my_free(b); // triple-merge: a absorbs b and c
    ASSERT_EQ(allocator::free_list_size_for_testing(), 1u);

    constexpr std::size_t kCombinedPayload = 3 * 64 + 2 * (sizeof(BlockFooter) + sizeof(BlockHeader));
    void* whole = allocator::my_malloc(kCombinedPayload);
    ASSERT_NE(whole, nullptr);
    EXPECT_EQ(whole, a);

    // If b or c had been left behind as stale, separately-reachable free
    // entries, the list would still have something in it (or a later
    // allocation could alias memory already handed out as part of
    // `whole`). Neither may happen.
    EXPECT_EQ(allocator::free_list_size_for_testing(), 0u);
}
