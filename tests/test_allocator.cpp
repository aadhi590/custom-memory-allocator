#include "allocator.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

namespace {

// Releases every arena back to the OS after each test so tests don't
// accumulate OS memory across the whole test binary run.
class AllocatorTest : public ::testing::Test {
protected:
    void TearDown() override { allocator::allocator_shutdown(); }
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

// Exercises crossing an arena boundary. The allocator's arena size is a
// fixed internal constant (1 MiB, see kArenaSize in allocator.cpp) that
// isn't currently injectable from tests -- adding a configuration knob
// purely to make this smaller felt like speculative complexity beyond what
// this phase asked for, so instead this test allocates well past a single
// arena's capacity (~2.4 MiB across many small allocations) to force at
// least one new arena request organically. Correctness of every
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

TEST_F(AllocatorTest, MyFreeIsANoOpAndDoesNotCrash) {
    void* ptr = allocator::my_malloc(64);
    ASSERT_NE(ptr, nullptr);
    std::memset(ptr, 0x42, 64);

    allocator::my_free(ptr);

    // Level 1 behavior: freeing does not reclaim the memory, so the
    // contents written before the free() call must still be intact and
    // still safe to read afterward.
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    for (std::size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(bytes[i], 0x42) << "byte " << i;
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
