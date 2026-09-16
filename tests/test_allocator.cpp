#include "allocator.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "block.hpp"

namespace {

// Releases every arena back to the OS after each test so tests don't
// accumulate OS memory across the whole test binary run.
class AllocatorTest : public ::testing::Test {
protected:
    void TearDown() override {
        allocator::allocator_shutdown();
        allocator::reset_arena_size_for_testing();
    }
};

} // namespace

TEST_F(AllocatorTest, ReturnsNonNullForVariousSizes) {
    EXPECT_NE(allocator::my_malloc(1), nullptr);
    EXPECT_NE(allocator::my_malloc(16), nullptr);
    EXPECT_NE(allocator::my_malloc(4096), nullptr);
}

TEST_F(AllocatorTest, ReturnedPointersAreAligned) {
    constexpr std::size_t sizes[] = {1, 3, 7, 15, 16, 17, 63, 100, 1000, 8192};
    for (std::size_t size : sizes) {
        void* ptr = allocator::my_malloc(size);
        ASSERT_NE(ptr, nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(ptr) % alignof(std::max_align_t), 0u)
            << "size " << size;
    }
}

TEST_F(AllocatorTest, AllocationsDoNotOverlap) {
    // Allocate many distinctly-sized blocks, stamp each with a pattern
    // derived from its index, then verify every block still holds its own
    // pattern at the end. If any two allocations overlapped in memory,
    // writing a later block's pattern would have corrupted an earlier one.
    constexpr int kCount = 200;
    std::vector<void*> ptrs;
    std::vector<std::size_t> sizes;
    ptrs.reserve(kCount);
    sizes.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
        const std::size_t size = static_cast<std::size_t>(1 + (i % 37) * 17);
        void* ptr = allocator::my_malloc(size);
        ASSERT_NE(ptr, nullptr);
        std::memset(ptr, static_cast<unsigned char>(i), size);
        ptrs.push_back(ptr);
        sizes.push_back(size);
    }

    for (int i = 0; i < kCount; ++i) {
        const auto* bytes = static_cast<const unsigned char*>(ptrs[static_cast<std::size_t>(i)]);
        for (std::size_t b = 0; b < sizes[static_cast<std::size_t>(i)]; ++b) {
            ASSERT_EQ(bytes[b], static_cast<unsigned char>(i)) << "allocation " << i << " byte " << b;
        }
    }
}

// Exercises crossing an arena boundary at the default (1 MiB) arena size,
// as an additional stress-style check alongside the deterministic
// ArenaRolloverIsDeterministicWithSmallArenaSize test below. This one
// allocates well past a single arena's capacity (~4.9 MiB across many
// allocations) to force at least one new arena request organically,
// rather than via set_arena_size_for_testing(). Correctness of every
// allocation's contents (same technique as AllocationsDoNotOverlap above)
// is what proves the new arena's blocks don't collide with the previous
// arena's blocks -- if the bump pointer or arena bookkeeping were wrong
// across that boundary, this test would corrupt data and fail.
//
// kChunkSize is deliberately > 4096 (the largest Phase 6 size class --
// see memory_pool.hpp): anything <= 4096 now routes to a pool instead of
// the general arena path this test is specifically exercising.
TEST_F(AllocatorTest, AllocationsSpanMultipleArenas) {
    constexpr std::size_t kChunkSize = 8192;
    constexpr int kCount = 600; // ~4.9 MiB total, > one 1 MiB arena
    std::vector<void*> ptrs;
    ptrs.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
        void* ptr = allocator::my_malloc(kChunkSize);
        ASSERT_NE(ptr, nullptr);
        std::memset(ptr, static_cast<unsigned char>(i & 0xFF), kChunkSize);
        ptrs.push_back(ptr);
    }

    for (int i = 0; i < kCount; ++i) {
        const auto* bytes = static_cast<const unsigned char*>(ptrs[static_cast<std::size_t>(i)]);
        for (std::size_t b = 0; b < kChunkSize; ++b) {
            ASSERT_EQ(bytes[b], static_cast<unsigned char>(i & 0xFF)) << "allocation " << i << " byte " << b;
        }
    }
}

// Deterministic counterpart to AllocationsSpanMultipleArenas: overrides the
// arena size to something small so multiple arena rollovers are forced
// reliably, then asserts directly (via arena_count_for_testing()) that
// multiple arenas were actually created, in addition to checking data
// correctness across the rollover boundaries.
//
// kPayload is deliberately > 4096 (the largest Phase 6 size class -- see
// memory_pool.hpp) so every my_malloc() call here exercises the general
// arena path this test is specifically exercising, not a pool. The forced
// arena size is set to hold only a couple of these (comparatively large)
// slots at a time -- os_acquire() page-rounds the actual request, but the
// exact resulting per-arena capacity doesn't need to be hand-derived:
// kCount is chosen generously large relative to "a couple of slots per
// arena" that several rollovers are guaranteed regardless of the exact
// page-rounding outcome.
TEST_F(AllocatorTest, ArenaRolloverIsDeterministicWithSmallArenaSize) {
    constexpr std::size_t kPayload = 4160; // > 4096; already alignof(std::max_align_t)-aligned
    // Every block reserves room for a BlockFooter as well as a
    // BlockHeader (Phase 4), so the per-allocation footprint includes both.
    const std::size_t needed_per_alloc = sizeof(allocator::BlockHeader) + kPayload + sizeof(allocator::BlockFooter);
    allocator::set_arena_size_for_testing(needed_per_alloc * 2);

    constexpr int kCount = 50; // comfortably forces several rollovers

    std::vector<void*> ptrs;
    ptrs.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
        void* ptr = allocator::my_malloc(kPayload);
        ASSERT_NE(ptr, nullptr);
        std::memset(ptr, static_cast<unsigned char>(i), kPayload);
        ptrs.push_back(ptr);
    }

    EXPECT_GE(allocator::arena_count_for_testing(), 3u);

    for (int i = 0; i < kCount; ++i) {
        const auto* bytes = static_cast<const unsigned char*>(ptrs[static_cast<std::size_t>(i)]);
        for (std::size_t b = 0; b < kPayload; ++b) {
            ASSERT_EQ(bytes[b], static_cast<unsigned char>(i)) << "allocation " << i << " byte " << b;
        }
    }
}

// The key proof that Phase 3 actually reuses memory rather than silently
// falling back to fresh arena space: freeing a block and immediately
// requesting the same size back must return the exact same address. Note
// that my_free() overwrites the freed block's former payload bytes with
// free-list link pointers (see FreeList::insert()), so this test does not
// expect the *contents* to survive the free -- only the *address*.
TEST_F(AllocatorTest, FreedBlockIsReusedAtTheSameAddressWhenSizeFits) {
    void* first = allocator::my_malloc(64);
    ASSERT_NE(first, nullptr);
    std::memset(first, 0xAB, 64);

    allocator::my_free(first);

    void* second = allocator::my_malloc(64);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second, first);

    std::memset(second, 0xCD, 64);
    const auto* bytes = static_cast<const unsigned char*>(second);
    for (std::size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(bytes[i], 0xCD) << "byte " << i;
    }
}

// Frees in a different order than allocation (allocate A, B, C; free B,
// then A) and verifies reuse still hands back exactly {A, B} with no
// corruption, while C -- never freed -- keeps its original contents
// throughout.
TEST_F(AllocatorTest, OutOfOrderFreeReusesCorrectlyWithoutCorruption) {
    void* a = allocator::my_malloc(64);
    void* b = allocator::my_malloc(64);
    void* c = allocator::my_malloc(64);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    std::memset(a, 0xAA, 64);
    std::memset(b, 0xBB, 64);
    std::memset(c, 0xCC, 64);

    allocator::my_free(b);
    allocator::my_free(a);

    void* reused1 = allocator::my_malloc(64);
    void* reused2 = allocator::my_malloc(64);
    ASSERT_NE(reused1, nullptr);
    ASSERT_NE(reused2, nullptr);

    // Both reused pointers must be exactly the two freed blocks, in some
    // order, and distinct from each other.
    EXPECT_NE(reused1, reused2);
    EXPECT_TRUE(reused1 == a || reused1 == b);
    EXPECT_TRUE(reused2 == a || reused2 == b);

    // c was never freed -- its contents must be untouched by any of the
    // free-list activity around it.
    const auto* c_bytes = static_cast<const unsigned char*>(c);
    for (std::size_t i = 0; i < 64; ++i) {
        ASSERT_EQ(c_bytes[i], 0xCC) << "byte " << i;
    }

    std::memset(reused1, 0x11, 64);
    std::memset(reused2, 0x22, 64);
    const auto* r1 = static_cast<const unsigned char*>(reused1);
    const auto* r2 = static_cast<const unsigned char*>(reused2);
    for (std::size_t i = 0; i < 64; ++i) {
        ASSERT_EQ(r1[i], 0x11) << "byte " << i;
        ASSERT_EQ(r2[i], 0x22) << "byte " << i;
    }
}

// A freed block that's too small for a later request must not be reused
// for it -- the request has to fall through to fresh arena/bump
// allocation instead. `large` is deliberately > 4096 so it exercises the
// general path rather than a pool.
TEST_F(AllocatorTest, RequestLargerThanAnyFreeBlockFallsThroughToArena) {
    void* small = allocator::my_malloc(16);
    ASSERT_NE(small, nullptr);
    allocator::my_free(small);

    void* large = allocator::my_malloc(5000);
    ASSERT_NE(large, nullptr);
    EXPECT_NE(large, small);

    std::memset(large, 0x99, 5000);
    const auto* bytes = static_cast<const unsigned char*>(large);
    for (std::size_t i = 0; i < 5000; ++i) {
        ASSERT_EQ(bytes[i], 0x99) << "byte " << i;
    }
}

TEST_F(AllocatorTest, AllocatorShutdownAllowsFreshAllocationsAfterward) {
    void* before = allocator::my_malloc(128);
    ASSERT_NE(before, nullptr);

    allocator::allocator_shutdown();

    void* after = allocator::my_malloc(128);
    ASSERT_NE(after, nullptr);
    std::memset(after, 0x1, 128);
}

// ---------------------------------------------------------------------------
// my_calloc
// ---------------------------------------------------------------------------

// The first allocation in a fresh test run comes straight from a
// freshly-mmap'd arena, never touched by the free list -- mmap's
// MAP_ANONYMOUS pages are already zeroed by the kernel, but calloc must
// still report zeroed memory regardless of how it got that way.
TEST_F(AllocatorTest, CallocZeroesFreshArenaMemory) {
    void* ptr = allocator::my_calloc(16, sizeof(std::uint64_t));
    ASSERT_NE(ptr, nullptr);

    const auto* bytes = static_cast<const unsigned char*>(ptr);
    for (std::size_t i = 0; i < 16 * sizeof(std::uint64_t); ++i) {
        EXPECT_EQ(bytes[i], 0) << "byte " << i;
    }
}

// calloc must zero memory even when it comes from a reused freed block,
// which can carry leftover FreeListLinks pointer bytes (from
// FreeList::insert() while it sat free) or a previous caller's stale
// payload data -- NOT just memory fresh from the OS. This forces the
// reuse case explicitly: dirty and free a block, then calloc a size that
// lands on that exact freed block, proven by checking the returned
// address matches.
TEST_F(AllocatorTest, CallocZeroesReusedFreedBlockMemory) {
    constexpr std::size_t kSize = 128;
    void* dirtied = allocator::my_malloc(kSize);
    ASSERT_NE(dirtied, nullptr);
    std::memset(dirtied, 0xFF, kSize);

    allocator::my_free(dirtied);

    void* reused = allocator::my_calloc(1, kSize);
    ASSERT_NE(reused, nullptr);
    ASSERT_EQ(reused, dirtied); // proves this really is the reused block

    const auto* bytes = static_cast<const unsigned char*>(reused);
    for (std::size_t i = 0; i < kSize; ++i) {
        EXPECT_EQ(bytes[i], 0) << "byte " << i;
    }
}

// count * size deliberately chosen to overflow size_t. Must return
// nullptr without crashing or corrupting allocator state -- verified by
// checking a normal allocation still works correctly afterward.
TEST_F(AllocatorTest, CallocOverflowReturnsNullptrSafely) {
    constexpr std::size_t kHugeCount = SIZE_MAX / 2 + 1;
    constexpr std::size_t kHugeSize = 4;

    void* ptr = allocator::my_calloc(kHugeCount, kHugeSize);
    EXPECT_EQ(ptr, nullptr);

    void* normal = allocator::my_malloc(64);
    ASSERT_NE(normal, nullptr);
    std::memset(normal, 0x5A, 64);
    const auto* bytes = static_cast<const unsigned char*>(normal);
    for (std::size_t i = 0; i < 64; ++i) {
        ASSERT_EQ(bytes[i], 0x5A) << "byte " << i;
    }
}

TEST_F(AllocatorTest, CallocWithZeroSizeOrCountDoesNotOverflowOrCrash) {
    EXPECT_NE(allocator::my_calloc(0, 64), nullptr);
    EXPECT_NE(allocator::my_calloc(64, 0), nullptr);
    EXPECT_NE(allocator::my_calloc(0, 0), nullptr);
}
