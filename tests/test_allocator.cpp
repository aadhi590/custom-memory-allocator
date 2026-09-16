#include "allocator.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unistd.h>
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
// allocates well past a single arena's capacity (~2.4 MiB across many
// small allocations) to force at least one new arena request organically,
// rather than via set_arena_size_for_testing(). Correctness of every
// allocation's contents (same technique as AllocationsDoNotOverlap above)
// is what proves the new arena's blocks don't collide with the previous
// arena's blocks -- if the bump pointer or arena bookkeeping were wrong
// across that boundary, this test would corrupt data and fail.
TEST_F(AllocatorTest, AllocationsSpanMultipleArenas) {
    constexpr std::size_t kChunkSize = 4096;
    constexpr int kCount = 600; // ~2.4 MiB total, > one 1 MiB arena
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
// arena size to something small so the exact number of arena rollovers is
// predictable, then asserts directly (via arena_count_for_testing()) that
// multiple arenas were actually created, in addition to checking data
// correctness across the rollover boundaries.
//
// The requested "small" arena size still gets page-rounded by os_acquire()
// (mmap always backs a request with whole pages), so this deliberately
// requests exactly one page rather than a literal small number like 256 --
// requesting less than a page would silently round back up to a full page
// and produce a much bigger, non-obvious effective arena than intended.
// The allocation count is computed from the real page size and payload
// size (rather than a hardcoded guess) so the test stays correct if run
// somewhere with a different page size.
TEST_F(AllocatorTest, ArenaRolloverIsDeterministicWithSmallArenaSize) {
    const auto page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    allocator::set_arena_size_for_testing(page_size);

    constexpr std::size_t kPayload = 64; // already alignof(std::max_align_t)-aligned
    const std::size_t needed_per_alloc = sizeof(allocator::BlockHeader) + kPayload;
    const std::size_t capacity_per_arena = page_size / needed_per_alloc;
    const int kCount = static_cast<int>(capacity_per_arena * 3 + 5); // force >= 3 rollovers

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
// allocation instead.
TEST_F(AllocatorTest, RequestLargerThanAnyFreeBlockFallsThroughToArena) {
    void* small = allocator::my_malloc(16);
    ASSERT_NE(small, nullptr);
    allocator::my_free(small);

    void* large = allocator::my_malloc(4096);
    ASSERT_NE(large, nullptr);
    EXPECT_NE(large, small);

    std::memset(large, 0x99, 4096);
    const auto* bytes = static_cast<const unsigned char*>(large);
    for (std::size_t i = 0; i < 4096; ++i) {
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
