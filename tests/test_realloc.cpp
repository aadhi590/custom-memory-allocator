#include "allocator.hpp"

#include <cstddef>
#include <cstring>

#include <gtest/gtest.h>

namespace {

class ReallocTest : public ::testing::Test {
protected:
    void TearDown() override {
        allocator::allocator_shutdown();
        allocator::reset_arena_size_for_testing();
    }
};

} // namespace

TEST_F(ReallocTest, ReallocNullptrBehavesLikeMalloc) {
    void* ptr = allocator::my_realloc(nullptr, 128);
    ASSERT_NE(ptr, nullptr);

    std::memset(ptr, 0x11, 128);
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    for (std::size_t i = 0; i < 128; ++i) {
        EXPECT_EQ(bytes[i], 0x11) << "byte " << i;
    }
}

// realloc(ptr, 0): documented as "return ptr unchanged" (see
// src/allocator.cpp's my_realloc() for the full reasoning and its
// consistency with my_malloc(0)). Locks in that choice as observable,
// tested behavior.
TEST_F(ReallocTest, ReallocToZeroReturnsSamePointerUnchanged) {
    void* ptr = allocator::my_malloc(64);
    ASSERT_NE(ptr, nullptr);
    std::memset(ptr, 0x22, 64);

    void* result = allocator::my_realloc(ptr, 0);
    EXPECT_EQ(result, ptr);

    const auto* bytes = static_cast<const unsigned char*>(result);
    for (std::size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(bytes[i], 0x22) << "byte " << i;
    }
}

TEST_F(ReallocTest, ReallocToSmallerSizeReturnsSamePointerWithPrefixIntact) {
    void* ptr = allocator::my_malloc(256);
    ASSERT_NE(ptr, nullptr);
    auto* bytes = static_cast<unsigned char*>(ptr);
    for (std::size_t i = 0; i < 256; ++i) {
        bytes[i] = static_cast<unsigned char>(i & 0xFF);
    }

    void* result = allocator::my_realloc(ptr, 64);
    EXPECT_EQ(result, ptr);

    const auto* result_bytes = static_cast<const unsigned char*>(result);
    for (std::size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(result_bytes[i], static_cast<unsigned char>(i & 0xFF)) << "byte " << i;
    }
}

// The key proof that grow-in-place actually happened, not just a copy
// that coincidentally preserved the data: asserts the SAME address is
// returned, not merely correct contents.
TEST_F(ReallocTest, ReallocGrowsInPlaceWhenRightNeighborIsFreeAndBigEnough) {
    void* a = allocator::my_malloc(64);
    void* b = allocator::my_malloc(64);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    std::memset(a, 0x33, 64);
    allocator::my_free(b); // a's right neighbor is now free, payload 64

    // align_up(100) == 112 > a's current 64, but combined with b
    // (64 + 16 + 16 + 64 = 160) is large enough -- must grow in place.
    void* grown = allocator::my_realloc(a, 100);

    ASSERT_EQ(grown, a); // same pointer -- proves grow-in-place, not a copy

    const auto* bytes = static_cast<const unsigned char*>(grown);
    for (std::size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(bytes[i], 0x33) << "byte " << i;
    }
}

// When grow-in-place isn't possible (right neighbor still allocated), a
// different pointer must be returned, data must be copied correctly, and
// the old block's memory must become available for reuse afterward.
TEST_F(ReallocTest, ReallocCopiesToNewBlockWhenGrowInPlaceNotPossible) {
    void* a = allocator::my_malloc(64);
    void* b = allocator::my_malloc(64); // keeps a's right neighbor allocated
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    std::memset(a, 0x44, 64);

    void* grown = allocator::my_realloc(a, 256);
    ASSERT_NE(grown, nullptr);
    EXPECT_NE(grown, a);

    const auto* bytes = static_cast<const unsigned char*>(grown);
    for (std::size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(bytes[i], 0x44) << "byte " << i;
    }

    // The old block (a) was freed as part of the fallback path -- a
    // subsequent allocation of its exact size must reuse its address.
    void* reused = allocator::my_malloc(64);
    EXPECT_EQ(reused, a);

    (void)b; // kept allocated only to block grow-in-place; not inspected further
}

// Forces the fallback my_malloc() inside my_realloc() to fail
// deterministically: a size far beyond any real virtual address space
// (2^56 bytes) makes the underlying mmap() call fail immediately at the
// address-space-reservation step (Linux x86-64's user address space tops
// out well below this), with no risk of actually consuming real memory or
// OOM-killing the test process. Verifies the original pointer and its
// contents remain completely untouched by the failed call.
TEST_F(ReallocTest, FailedReallocLeavesOriginalPointerAndDataUntouched) {
    void* a = allocator::my_malloc(64);
    void* b = allocator::my_malloc(64); // keeps a's right neighbor allocated
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    std::memset(a, 0x55, 64);

    constexpr std::size_t kImpossibleSize = std::size_t{1} << 56;
    void* result = allocator::my_realloc(a, kImpossibleSize);

    EXPECT_EQ(result, nullptr);

    const auto* bytes = static_cast<const unsigned char*>(a);
    for (std::size_t i = 0; i < 64; ++i) {
        ASSERT_EQ(bytes[i], 0x55) << "byte " << i;
    }

    // `a` must still be a valid, unfreed allocation -- freeing it now
    // must work cleanly (it would be a double-free / corruption if the
    // failed realloc had freed it internally, contrary to the contract).
    allocator::my_free(a);
}

// Growing in place when the free right neighbor is much larger than
// needed must split the excess back into the free list, not silently
// swallow the whole neighbor into the returned block.
TEST_F(ReallocTest, ReallocGrowInPlaceSplitsExcessBackToFreeList) {
    void* a = allocator::my_malloc(64);
    void* b = allocator::my_malloc(512);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    std::memset(a, 0x66, 64);
    allocator::my_free(b); // a's right neighbor is free, payload 512

    // align_up(100) == 112, comfortably less than the full combined
    // 64 + 16 + 16 + 512 = 608 available.
    void* grown = allocator::my_realloc(a, 100);
    ASSERT_EQ(grown, a);

    // The returned block's reported size must match what was requested
    // (rounded up), not the full neighbor size -- proves the excess was
    // split off rather than swallowed whole.
    EXPECT_EQ(allocator::block_payload_size_for_testing(grown), 112u);

    // The split-off remainder must be genuinely reachable through the
    // free list, and must not overlap the grown block.
    void* remainder_reuse = allocator::my_malloc(64);
    EXPECT_NE(remainder_reuse, nullptr);
    EXPECT_NE(remainder_reuse, grown);

    const auto* bytes = static_cast<const unsigned char*>(grown);
    for (std::size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(bytes[i], 0x66) << "byte " << i;
    }
}
