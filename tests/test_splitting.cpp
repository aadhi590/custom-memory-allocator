#include "allocator.hpp"

#include <cstddef>
#include <cstring>

#include <gtest/gtest.h>

#include "block.hpp"

namespace {

class SplittingTest : public ::testing::Test {
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

// Request 100 bytes against a 512-byte free block: the requested size
// rounds up to align_up(100) = 112, leaving a remainder of
// 512 - 112 = 400 bytes -- comfortably larger than kMinBlockSize, so a
// split must occur. Verifies both resulting blocks' headers and footers,
// and that the remainder is genuinely reachable through the free list
// afterward (not just correctly laid out in memory).
TEST_F(SplittingTest, LargeFreeBlockSplitsToSatisfySmallerRequest) {
    void* big = allocator::my_malloc(512);
    ASSERT_NE(big, nullptr);
    std::memset(big, 0xEE, 512);

    allocator::my_free(big);

    void* small = allocator::my_malloc(100);
    ASSERT_NE(small, nullptr);
    // Same starting address as the free block that was split -- proves
    // reuse actually happened rather than falling back to a fresh arena.
    EXPECT_EQ(small, big);

    BlockHeader* small_header = header_of(small);
    constexpr std::size_t kExpectedSmallPayload = 112; // align_up(100)
    EXPECT_EQ(small_header->get_size(), kExpectedSmallPayload);
    EXPECT_FALSE(small_header->is_free());
    EXPECT_EQ(small_header->footer()->size, kExpectedSmallPayload);

    BlockHeader* remainder_header = small_header->next_physical_header();
    constexpr std::size_t kExpectedRemainderPayload =
        512 - kExpectedSmallPayload - sizeof(BlockHeader) - sizeof(BlockFooter);
    EXPECT_TRUE(remainder_header->is_free());
    EXPECT_EQ(remainder_header->get_size(), kExpectedRemainderPayload);
    EXPECT_EQ(remainder_header->footer()->size, kExpectedRemainderPayload);

    // The remainder block is genuinely reachable through the free list --
    // a request that only it (not any smaller/other block) can satisfy
    // must return exactly its address.
    void* remainder_payload_addr = reinterpret_cast<std::byte*>(remainder_header) + sizeof(BlockHeader);
    void* reused_remainder = allocator::my_malloc(kExpectedRemainderPayload);
    EXPECT_EQ(reused_remainder, remainder_payload_addr);
}

// When the leftover after carving out the requested size would be smaller
// than kMinBlockSize, splitting must be skipped entirely: the whole
// (oversized) block is handed over, unshrunk.
TEST_F(SplittingTest, RemainderSmallerThanMinBlockSizeSkipsSplitting) {
    void* original = allocator::my_malloc(128);
    ASSERT_NE(original, nullptr);
    allocator::my_free(original);

    // payload_size for 100 is align_up(100) = 112; remainder = 128 - 112 =
    // 16, which is smaller than kMinBlockSize (48: header + FreeListLinks
    // + footer) -- must not split.
    void* reused = allocator::my_malloc(100);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused, original);

    BlockHeader* header = header_of(reused);
    EXPECT_EQ(header->get_size(), 128u);
    EXPECT_EQ(header->footer()->size, 128u);
}

// Boundary case: a remainder exactly equal to kMinBlockSize must still
// split (the condition is >=, not >).
TEST_F(SplittingTest, RemainderExactlyMinBlockSizeStillSplits) {
    void* original = allocator::my_malloc(160);
    ASSERT_NE(original, nullptr);
    allocator::my_free(original);

    // payload_size for 100 is 112; remainder = 160 - 112 = 48 ==
    // kMinBlockSize exactly.
    void* reused = allocator::my_malloc(100);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused, original);

    BlockHeader* header = header_of(reused);
    EXPECT_EQ(header->get_size(), 112u); // shrunk -- split occurred

    BlockHeader* remainder_header = header->next_physical_header();
    EXPECT_TRUE(remainder_header->is_free());
    EXPECT_EQ(remainder_header->get_size(), 16u); // exactly sizeof(FreeListLinks)
    EXPECT_EQ(remainder_header->footer()->size, 16u);
}

// The two blocks resulting from a split must not overlap: writing through
// one must never disturb the other, whether the remainder is still free
// or has since been reused.
TEST_F(SplittingTest, SplitBlocksDoNotOverlap) {
    void* original = allocator::my_malloc(512);
    ASSERT_NE(original, nullptr);
    allocator::my_free(original);

    void* small = allocator::my_malloc(100);
    ASSERT_NE(small, nullptr);
    std::memset(small, 0xAB, 100);

    BlockHeader* small_header = header_of(small);
    BlockHeader* remainder_header = small_header->next_physical_header();
    const std::size_t remainder_payload = remainder_header->get_size();

    void* remainder_ptr = allocator::my_malloc(remainder_payload);
    ASSERT_NE(remainder_ptr, nullptr);
    std::memset(remainder_ptr, 0xCD, remainder_payload);

    const auto* small_bytes = static_cast<const unsigned char*>(small);
    for (std::size_t i = 0; i < 100; ++i) {
        ASSERT_EQ(small_bytes[i], 0xAB) << "small byte " << i;
    }

    const auto* remainder_bytes = static_cast<const unsigned char*>(remainder_ptr);
    for (std::size_t i = 0; i < remainder_payload; ++i) {
        ASSERT_EQ(remainder_bytes[i], 0xCD) << "remainder byte " << i;
    }
}
