#include "memory_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

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
