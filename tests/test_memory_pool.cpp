#include "memory_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

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
