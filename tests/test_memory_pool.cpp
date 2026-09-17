#include "memory_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "allocator.hpp"

// ---------------------------------------------------------------------------
// size_class_for() boundary correctness
// ---------------------------------------------------------------------------

class SizeClassForTest : public ::testing::TestWithParam<std::pair<std::size_t, std::size_t>> {};

TEST_P(SizeClassForTest, RoundsUpToExpectedClass) {
    const auto [requested, expected] = GetParam();
    EXPECT_EQ(allocator::size_class_for(requested), expected) << "requested = " << requested;
}

INSTANTIATE_TEST_SUITE_P(
    Boundaries, SizeClassForTest,
    ::testing::Values(
        // Degenerate/small inputs -- all round up to the smallest class.
        std::pair<std::size_t, std::size_t>{0, 16},
        std::pair<std::size_t, std::size_t>{1, 16},
        std::pair<std::size_t, std::size_t>{15, 16},
        std::pair<std::size_t, std::size_t>{16, 16}, // exact match: a class boundary belongs to that class
        std::pair<std::size_t, std::size_t>{17, 32}, // one past a boundary rounds up to the NEXT class

        std::pair<std::size_t, std::size_t>{31, 32},
        std::pair<std::size_t, std::size_t>{32, 32},
        std::pair<std::size_t, std::size_t>{33, 64},

        std::pair<std::size_t, std::size_t>{63, 64},
        std::pair<std::size_t, std::size_t>{64, 64},
        std::pair<std::size_t, std::size_t>{65, 128},

        std::pair<std::size_t, std::size_t>{100, 128}, // the phase brief's own worked example
        std::pair<std::size_t, std::size_t>{127, 128},
        std::pair<std::size_t, std::size_t>{128, 128},
        std::pair<std::size_t, std::size_t>{129, 256},

        std::pair<std::size_t, std::size_t>{255, 256},
        std::pair<std::size_t, std::size_t>{256, 256},
        std::pair<std::size_t, std::size_t>{257, 512},

        std::pair<std::size_t, std::size_t>{511, 512},
        std::pair<std::size_t, std::size_t>{512, 512},
        std::pair<std::size_t, std::size_t>{513, 1024},

        std::pair<std::size_t, std::size_t>{1023, 1024},
        std::pair<std::size_t, std::size_t>{1024, 1024},
        std::pair<std::size_t, std::size_t>{1025, 2048},

        std::pair<std::size_t, std::size_t>{2047, 2048},
        std::pair<std::size_t, std::size_t>{2048, 2048},
        std::pair<std::size_t, std::size_t>{2049, 4096},

        // The top boundary: exactly the largest class still fits; one
        // byte more must fall through to the general path (kNoSizeClass).
        std::pair<std::size_t, std::size_t>{4095, 4096},
        std::pair<std::size_t, std::size_t>{4096, 4096},
        std::pair<std::size_t, std::size_t>{4097, allocator::kNoSizeClass},

        std::pair<std::size_t, std::size_t>{5000, allocator::kNoSizeClass},
        std::pair<std::size_t, std::size_t>{1u << 20, allocator::kNoSizeClass},
        std::pair<std::size_t, std::size_t>{SIZE_MAX, allocator::kNoSizeClass}));

// ---------------------------------------------------------------------------
// Pool allocate/deallocate basic correctness
//
// Pool objects in these tests are local (not shared allocator state), so
// there's no fixture/TearDown to wire up -- each test explicitly calls
// release_all_arenas() at the end so the test binary doesn't leak OS
// memory across the whole run.
// ---------------------------------------------------------------------------

TEST(PoolTest, AllocateReturnsWritableSlotAndDeallocateAllowsReuse) {
    allocator::Pool pool(64);

    void* first = pool.allocate();
    ASSERT_NE(first, nullptr);
    std::memset(first, 0xAB, 64);

    pool.deallocate(first);

    void* second = pool.allocate();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second, first); // same address reused -- proves the free-slot list works

    pool.release_all_arenas();
}

TEST(PoolTest, MultipleAllocationsDoNotOverlap) {
    allocator::Pool pool(32);
    constexpr int kCount = 50;
    std::vector<void*> ptrs;
    ptrs.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
        void* ptr = pool.allocate();
        ASSERT_NE(ptr, nullptr);
        std::memset(ptr, static_cast<unsigned char>(i), 32);
        ptrs.push_back(ptr);
    }

    for (int i = 0; i < kCount; ++i) {
        const auto* bytes = static_cast<const unsigned char*>(ptrs[static_cast<std::size_t>(i)]);
        for (std::size_t b = 0; b < 32; ++b) {
            ASSERT_EQ(bytes[b], static_cast<unsigned char>(i)) << "allocation " << i << " byte " << b;
        }
    }

    pool.release_all_arenas();
}

TEST(PoolTest, DeallocatingOutOfOrderStillAllowsCorrectReuse) {
    allocator::Pool pool(16);

    void* a = pool.allocate();
    void* b = pool.allocate();
    void* c = pool.allocate();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    std::memset(a, 0xAA, 16);
    std::memset(b, 0xBB, 16);
    std::memset(c, 0xCC, 16);

    pool.deallocate(b); // free the middle one
    pool.deallocate(a); // then the first one

    void* reused1 = pool.allocate();
    void* reused2 = pool.allocate();
    ASSERT_NE(reused1, nullptr);
    ASSERT_NE(reused2, nullptr);

    EXPECT_NE(reused1, reused2);
    EXPECT_TRUE(reused1 == a || reused1 == b);
    EXPECT_TRUE(reused2 == a || reused2 == b);

    // c was never freed -- must still hold its original contents.
    const auto* c_bytes = static_cast<const unsigned char*>(c);
    for (std::size_t i = 0; i < 16; ++i) {
        ASSERT_EQ(c_bytes[i], 0xCC) << "byte " << i;
    }

    pool.release_all_arenas();
}

// ---------------------------------------------------------------------------
// Pool expansion
// ---------------------------------------------------------------------------

// Exhausts a pool's initial arena (allocating well past a single arena's
// slot capacity, without depending on the exact per-arena slot count) and
// verifies a second arena gets requested, with allocation continuing to
// work correctly -- no overlap, no corruption -- across the expansion
// boundary.
TEST(PoolTest, ExhaustingInitialArenaTriggersExpansionAndContinuesWorking) {
    allocator::Pool pool(16); // smallest class; one arena holds ~256 slots

    constexpr int kCount = 600; // comfortably more than one arena can hold
    std::vector<void*> ptrs;
    ptrs.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
        void* ptr = pool.allocate();
        ASSERT_NE(ptr, nullptr);
        std::memset(ptr, static_cast<unsigned char>(i & 0xFF), 16);
        ptrs.push_back(ptr);
    }

    EXPECT_GE(pool.arena_count_for_testing(), 2u);

    for (int i = 0; i < kCount; ++i) {
        const auto* bytes = static_cast<const unsigned char*>(ptrs[static_cast<std::size_t>(i)]);
        for (std::size_t b = 0; b < 16; ++b) {
            ASSERT_EQ(bytes[b], static_cast<unsigned char>(i & 0xFF)) << "allocation " << i << " byte " << b;
        }
    }

    pool.release_all_arenas();
}

// ---------------------------------------------------------------------------
// Pool::allocate_batch() / deallocate_batch() (Phase 7, Stage 3 support)
// ---------------------------------------------------------------------------

TEST(PoolTest, AllocateBatchReturnsRequestedCountAsAValidChain) {
    allocator::Pool pool(64);

    auto batch = pool.allocate_batch(10);
    EXPECT_EQ(batch.count, 10u);
    ASSERT_NE(batch.head, nullptr);
    ASSERT_NE(batch.tail, nullptr);

    // Walk the chain and confirm it really has exactly `count` nodes,
    // ending at `tail`, with next_free (not owning_pool) still valid --
    // i.e. still in "free" format, not marked allocated.
    allocator::PoolSlotHeader* node = batch.head;
    std::size_t walked = 1;
    while (node->next_free != nullptr) {
        node = node->next_free;
        ++walked;
    }
    EXPECT_EQ(walked, batch.count);
    EXPECT_EQ(node, batch.tail);

    pool.release_all_arenas();
}

TEST(PoolTest, AllocateBatchSlotsAreIndividuallyUsableAfterClaiming) {
    allocator::Pool pool(32);

    auto batch = pool.allocate_batch(5);
    ASSERT_EQ(batch.count, 5u);

    // Claim each slot individually (mirroring how allocator.cpp's
    // thread-local cache layer hands one out at a time), write through
    // it, and verify no overlap.
    std::vector<void*> ptrs;
    allocator::PoolSlotHeader* node = batch.head;
    for (std::size_t i = 0; i < batch.count; ++i) {
        allocator::PoolSlotHeader* next = node->next_free;
        void* payload = reinterpret_cast<std::byte*>(node) + sizeof(allocator::PoolSlotHeader);
        std::memset(payload, static_cast<unsigned char>(i), 32);
        ptrs.push_back(payload);
        node = next;
    }

    for (std::size_t i = 0; i < ptrs.size(); ++i) {
        const auto* bytes = static_cast<const unsigned char*>(ptrs[i]);
        for (std::size_t b = 0; b < 32; ++b) {
            ASSERT_EQ(bytes[b], static_cast<unsigned char>(i)) << "slot " << i << " byte " << b;
        }
    }

    pool.release_all_arenas();
}

TEST(PoolTest, DeallocateBatchSpliceMakesSlotsReusableViaAllocate) {
    allocator::Pool pool(16);

    auto batch = pool.allocate_batch(8);
    ASSERT_EQ(batch.count, 8u);

    // Return the whole batch at once via the O(1) splice.
    pool.deallocate_batch(batch.head, batch.tail);

    // All 8 slots must now be reachable again through ordinary
    // allocate() calls.
    std::vector<void*> ptrs;
    for (int i = 0; i < 8; ++i) {
        void* ptr = pool.allocate();
        ASSERT_NE(ptr, nullptr);
        ptrs.push_back(ptr);
    }
    // No duplicates -- confirms the spliced chain wasn't corrupted (e.g.
    // a cycle, or a lost node) by the batch round trip.
    for (std::size_t i = 0; i < ptrs.size(); ++i) {
        for (std::size_t j = i + 1; j < ptrs.size(); ++j) {
            EXPECT_NE(ptrs[i], ptrs[j]) << "duplicate slot at " << i << " and " << j;
        }
    }

    pool.release_all_arenas();
}

TEST(PoolTest, AllocateBatchExpandsAcrossArenaBoundaryWhenNeeded) {
    allocator::Pool pool(16); // one arena holds ~256 slots

    // Request more slots than a single arena holds, forcing expand()
    // partway through the batch.
    auto batch = pool.allocate_batch(600);
    EXPECT_EQ(batch.count, 600u);
    EXPECT_GE(pool.arena_count_for_testing(), 2u);

    pool.release_all_arenas();
}

// ---------------------------------------------------------------------------
// Pointer-to-pool routing (through the public allocator API)
//
// These exercise allocator.cpp's my_malloc()/my_free() routing directly,
// mixing several different pools' size classes with the general
// free-list/arena path, to catch a routing bug -- a pooled pointer
// misrouted into general-path coalescing, or vice versa -- that would
// otherwise manifest as silent heap corruption rather than a crash.
// ---------------------------------------------------------------------------

namespace {

class RoutingTest : public ::testing::Test {
protected:
    void TearDown() override { allocator::allocator_shutdown(); }
};

} // namespace

// Allocates from several different size classes (16 through 4096) plus
// several general-path sizes (> 4096), all interleaved, then frees every
// single one of them in a deliberately shuffled order (not allocation
// order, not reverse order). Correctness is proven the same way as
// Phase 3-5's overlap tests: every allocation is stamped with a distinct
// pattern before any frees happen, verified intact right before freeing
// starts, and then -- after everything has been freed in shuffled order
// -- fresh allocations of the exact same sizes are made and verified
// independently writable and non-overlapping. A misrouted free anywhere
// in the shuffled sequence would corrupt either a pool's internal
// free-slot list or the general free list, and that corruption would
// surface here as a crash, overlapping addresses, or a wrong pattern.
TEST_F(RoutingTest, MixedPooledAndGeneralPathPointersRouteToCorrectDeallocationPath) {
    struct Alloc {
        void* ptr;
        std::size_t size;
        unsigned char pattern;
    };

    // Several different size classes (16, 32, 64, 256, 1024, 4096) plus
    // several general-path sizes (5000, 6000, 8192, 10000), interleaved.
    const std::vector<std::size_t> sizes = {16, 5000, 64, 10000, 256, 1024, 8192, 4096, 32, 2048, 6000, 512};

    std::vector<Alloc> allocs;
    allocs.reserve(sizes.size());
    unsigned char pattern = 0x01;
    for (std::size_t size : sizes) {
        void* ptr = allocator::my_malloc(size);
        ASSERT_NE(ptr, nullptr) << "size " << size;
        std::memset(ptr, pattern, size);
        allocs.push_back(Alloc{ptr, size, pattern});
        ++pattern;
    }

    // Verify nothing overlaps before any frees happen.
    for (const auto& a : allocs) {
        const auto* bytes = static_cast<const unsigned char*>(a.ptr);
        for (std::size_t i = 0; i < a.size; ++i) {
            ASSERT_EQ(bytes[i], a.pattern) << "size " << a.size << " byte " << i;
        }
    }

    // A fixed, manually-chosen permutation (not allocation order, not
    // reverse order) so the test is deterministic without needing
    // <random> or <algorithm>.
    const std::vector<std::size_t> free_order = {5, 0, 11, 2, 8, 1, 9, 4, 6, 10, 3, 7};
    ASSERT_EQ(free_order.size(), allocs.size());

    for (std::size_t idx : free_order) {
        allocator::my_free(allocs[idx].ptr);
    }

    // Fresh allocations of the same sizes must all succeed and be
    // independently writable -- proving every free above was routed
    // correctly.
    std::vector<Alloc> reallocs;
    reallocs.reserve(sizes.size());
    pattern = 0x81;
    for (std::size_t size : sizes) {
        void* ptr = allocator::my_malloc(size);
        ASSERT_NE(ptr, nullptr) << "size " << size;
        std::memset(ptr, pattern, size);
        reallocs.push_back(Alloc{ptr, size, pattern});
        ++pattern;
    }

    for (const auto& a : reallocs) {
        const auto* bytes = static_cast<const unsigned char*>(a.ptr);
        for (std::size_t i = 0; i < a.size; ++i) {
            ASSERT_EQ(bytes[i], a.pattern) << "size " << a.size << " byte " << i;
        }
    }
}

// A more precise companion to the test above: proves each pointer is
// reused at exactly its OWN previously-freed address after a shuffled
// free order, which only holds if each free was routed to the correct
// path (a pooled pointer misrouted into the general free list, or vice
// versa, would not reliably reproduce the original address). Uses only
// one general-path pointer deliberately, to avoid the general path's
// (entirely correct) coalescing of physically-adjacent free blocks from
// complicating the "same address" assertions -- that coalescing behavior
// is already covered by test_coalescing.cpp.
TEST_F(RoutingTest, FreeingMixedPointersReusesEachAtItsOwnCorrectAddress) {
    void* pooled_a = allocator::my_malloc(64);
    void* general_a = allocator::my_malloc(5000);
    void* pooled_b = allocator::my_malloc(256);
    ASSERT_NE(pooled_a, nullptr);
    ASSERT_NE(general_a, nullptr);
    ASSERT_NE(pooled_b, nullptr);

    // Shuffled free order.
    allocator::my_free(pooled_b);
    allocator::my_free(general_a);
    allocator::my_free(pooled_a);

    void* pooled_a2 = allocator::my_malloc(64);
    void* pooled_b2 = allocator::my_malloc(256);
    void* general_a2 = allocator::my_malloc(5000);

    EXPECT_EQ(pooled_a2, pooled_a);
    EXPECT_EQ(pooled_b2, pooled_b);
    EXPECT_EQ(general_a2, general_a);
}

// Regression check that Phase 6 didn't break Phase 3-5's existing
// behavior for large allocations: a size within a size class is
// identified as pooled, and a size exceeding the largest size class
// still uses the general path exactly as before, verified directly via
// ptr_is_pooled_for_testing() rather than only inferred from behavior.
TEST_F(RoutingTest, SmallSizeIsPooledLargeSizeUsesGeneralPathUnchanged) {
    void* small = allocator::my_malloc(100); // rounds up to the 128-byte class
    ASSERT_NE(small, nullptr);
    EXPECT_TRUE(allocator::ptr_is_pooled_for_testing(small));

    void* exact_class = allocator::my_malloc(4096); // exactly the largest class
    ASSERT_NE(exact_class, nullptr);
    EXPECT_TRUE(allocator::ptr_is_pooled_for_testing(exact_class));

    void* large = allocator::my_malloc(4097); // one byte over -- general path
    ASSERT_NE(large, nullptr);
    EXPECT_FALSE(allocator::ptr_is_pooled_for_testing(large));

    void* very_large = allocator::my_malloc(1u << 20); // 1 MiB -- general path
    ASSERT_NE(very_large, nullptr);
    EXPECT_FALSE(allocator::ptr_is_pooled_for_testing(very_large));
}
