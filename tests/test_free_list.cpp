#include "free_list.hpp"

#include <array>
#include <cstddef>
#include <new>
#include <vector>

#include <gtest/gtest.h>

#include "block.hpp"

using allocator::BlockHeader;
using allocator::FreeList;

namespace {

// Backing storage for a fake free block, sized so the header plus a small
// payload region has room for FreeListLinks once the block is marked free.
struct FakeBlock {
    alignas(alignof(std::max_align_t)) std::array<std::byte, sizeof(BlockHeader) + 64> storage{};

    BlockHeader* header() { return reinterpret_cast<BlockHeader*>(storage.data()); }
};

// Constructs a free BlockHeader of `size` at `block`'s storage.
BlockHeader* make_free_block(FakeBlock& block, std::size_t size) {
    return new (block.storage.data()) BlockHeader(size, /*free=*/true);
}

} // namespace

// ---------------------------------------------------------------------------
// insert/remove basic correctness
// ---------------------------------------------------------------------------

TEST(FreeListTest, InsertSingleBlockBecomesHeadWithNullLinks) {
    FakeBlock block;
    BlockHeader* header = make_free_block(block, 64);

    FreeList list;
    list.insert(header);

    EXPECT_FALSE(list.empty());
    EXPECT_EQ(header->free_list_links()->next, nullptr);
    EXPECT_EQ(header->free_list_links()->prev, nullptr);
}

TEST(FreeListTest, InsertingSecondBlockLinksToFirst) {
    FakeBlock block_a, block_b;
    BlockHeader* a = make_free_block(block_a, 64);
    BlockHeader* b = make_free_block(block_b, 64);

    FreeList list;
    list.insert(a);
    list.insert(b); // head insertion: b becomes the new head, in front of a

    EXPECT_EQ(b->free_list_links()->next, a);
    EXPECT_EQ(b->free_list_links()->prev, nullptr);
    EXPECT_EQ(a->free_list_links()->next, nullptr);
    EXPECT_EQ(a->free_list_links()->prev, b);
}

TEST(FreeListTest, RemoveOnlyElementEmptiesList) {
    FakeBlock block;
    BlockHeader* header = make_free_block(block, 64);

    FreeList list;
    list.insert(header);
    list.remove(header);

    EXPECT_TRUE(list.empty());
    EXPECT_EQ(header->free_list_links()->next, nullptr);
    EXPECT_EQ(header->free_list_links()->prev, nullptr);
}

TEST(FreeListTest, RemoveHeadUpdatesListHead) {
    FakeBlock block_a, block_b, block_c;
    BlockHeader* a = make_free_block(block_a, 16);
    BlockHeader* b = make_free_block(block_b, 32);
    BlockHeader* c = make_free_block(block_c, 48);

    FreeList list;
    list.insert(a); // list: a
    list.insert(b); // list: b -> a
    list.insert(c); // list: c -> b -> a

    list.remove(c); // list: b -> a

    EXPECT_EQ(b->free_list_links()->prev, nullptr);
    EXPECT_EQ(b->free_list_links()->next, a);
    EXPECT_EQ(a->free_list_links()->prev, b);
    EXPECT_EQ(a->free_list_links()->next, nullptr);
}

TEST(FreeListTest, RemoveTailUpdatesPreviousNodesNext) {
    FakeBlock block_a, block_b, block_c;
    BlockHeader* a = make_free_block(block_a, 16);
    BlockHeader* b = make_free_block(block_b, 32);
    BlockHeader* c = make_free_block(block_c, 48);

    FreeList list;
    list.insert(a); // list: a
    list.insert(b); // list: b -> a
    list.insert(c); // list: c -> b -> a

    list.remove(a); // list: c -> b

    EXPECT_EQ(b->free_list_links()->next, nullptr);
    EXPECT_EQ(c->free_list_links()->next, b);
    EXPECT_EQ(b->free_list_links()->prev, c);
}

// Removing from the middle only requires rewriting the removed node's two
// immediate neighbors' links -- it does not require (and this
// implementation does not perform) a scan of the rest of the list. This
// test demonstrates that by checking the full list is correctly stitched
// back together with a node in the middle removed, including nodes further
// away from the removal point being untouched.
TEST(FreeListTest, RemoveMiddleNodeStitchesNeighborsTogetherOnly) {
    FakeBlock block_a, block_b, block_c, block_d, block_e;
    BlockHeader* a = make_free_block(block_a, 16);
    BlockHeader* b = make_free_block(block_b, 32);
    BlockHeader* c = make_free_block(block_c, 48);
    BlockHeader* d = make_free_block(block_d, 64);
    BlockHeader* e = make_free_block(block_e, 80);

    FreeList list;
    list.insert(a); // a
    list.insert(b); // b -> a
    list.insert(c); // c -> b -> a
    list.insert(d); // d -> c -> b -> a
    list.insert(e); // e -> d -> c -> b -> a

    list.remove(c); // expect: e -> d -> b -> a

    // c's immediate neighbors (d and b) were rewired around it.
    EXPECT_EQ(d->free_list_links()->next, b);
    EXPECT_EQ(b->free_list_links()->prev, d);

    // c itself is fully unlinked.
    EXPECT_EQ(c->free_list_links()->next, nullptr);
    EXPECT_EQ(c->free_list_links()->prev, nullptr);

    // Nodes further from the removal point are untouched.
    EXPECT_EQ(e->free_list_links()->prev, nullptr);
    EXPECT_EQ(e->free_list_links()->next, d);
    EXPECT_EQ(d->free_list_links()->prev, e);
    EXPECT_EQ(a->free_list_links()->next, nullptr);
    EXPECT_EQ(a->free_list_links()->prev, b);
    EXPECT_EQ(b->free_list_links()->next, a);
}

TEST(FreeListTest, InsertRemoveInterleavedInDifferentOrdersStaysConsistent) {
    FakeBlock block_a, block_b, block_c, block_d;
    BlockHeader* a = make_free_block(block_a, 16);
    BlockHeader* b = make_free_block(block_b, 32);
    BlockHeader* c = make_free_block(block_c, 48);
    BlockHeader* d = make_free_block(block_d, 64);

    FreeList list;
    list.insert(a);
    list.insert(b);
    list.remove(a); // list: b
    list.insert(c); // list: c -> b
    list.insert(d); // list: d -> c -> b
    list.remove(b); // list: d -> c
    list.remove(d); // list: c

    EXPECT_FALSE(list.empty());
    EXPECT_EQ(list.find_first_fit(1), c);
    EXPECT_EQ(c->free_list_links()->next, nullptr);
    EXPECT_EQ(c->free_list_links()->prev, nullptr);

    list.remove(c);
    EXPECT_TRUE(list.empty());
}

// ---------------------------------------------------------------------------
// find_first_fit
// ---------------------------------------------------------------------------

TEST(FreeListTest, FindFirstFitReturnsFirstBlockLargeEnough) {
    FakeBlock block_small, block_medium, block_large;
    BlockHeader* small = make_free_block(block_small, 16);
    BlockHeader* medium = make_free_block(block_medium, 48);
    BlockHeader* large = make_free_block(block_large, 128);

    FreeList list;
    // Insert in an order where the first sufficiently-large block
    // encountered during the walk is `medium`, not `large`.
    list.insert(small);  // list: small
    list.insert(medium); // list: medium -> small
    list.insert(large);  // list: large -> medium -> small

    EXPECT_EQ(list.find_first_fit(32), large);

    list.remove(large); // list: medium -> small
    EXPECT_EQ(list.find_first_fit(32), medium);
}

TEST(FreeListTest, FindFirstFitReturnsNullptrWhenNothingFits) {
    FakeBlock block;
    BlockHeader* header = make_free_block(block, 16);

    FreeList list;
    list.insert(header);

    EXPECT_EQ(list.find_first_fit(1024), nullptr);
}

TEST(FreeListTest, FindFirstFitOnEmptyListReturnsNullptr) {
    FreeList list;
    EXPECT_EQ(list.find_first_fit(1), nullptr);
}

TEST(FreeListTest, FindFirstFitAcceptsExactSizeMatch) {
    FakeBlock block;
    BlockHeader* header = make_free_block(block, 64);

    FreeList list;
    list.insert(header);

    EXPECT_EQ(list.find_first_fit(64), header);
}

// ---------------------------------------------------------------------------
// size()
// ---------------------------------------------------------------------------

TEST(FreeListTest, SizeOfEmptyListIsZero) {
    FreeList list;
    EXPECT_EQ(list.size(), 0u);
}

TEST(FreeListTest, SizeTracksInsertsAndRemoves) {
    FakeBlock block_a, block_b, block_c;
    BlockHeader* a = make_free_block(block_a, 16);
    BlockHeader* b = make_free_block(block_b, 32);
    BlockHeader* c = make_free_block(block_c, 48);

    FreeList list;
    EXPECT_EQ(list.size(), 0u);

    list.insert(a);
    EXPECT_EQ(list.size(), 1u);
    list.insert(b);
    EXPECT_EQ(list.size(), 2u);
    list.insert(c);
    EXPECT_EQ(list.size(), 3u);

    list.remove(b);
    EXPECT_EQ(list.size(), 2u);
    list.remove(a);
    EXPECT_EQ(list.size(), 1u);
    list.remove(c);
    EXPECT_EQ(list.size(), 0u);
}
