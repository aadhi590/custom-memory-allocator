#include "block.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>

#include <gtest/gtest.h>

using allocator::BlockFooter;
using allocator::BlockHeader;
using allocator::FreeListLinks;

// ---------------------------------------------------------------------------
// 1. sizeof/alignof expectations (runtime-visible counterpart to the
//    static_asserts in block.hpp, so they show up in CTest/gtest output too).
// ---------------------------------------------------------------------------

TEST(BlockLayout, HeaderSizeIsMultipleOfMaxAlign) {
    EXPECT_EQ(sizeof(BlockHeader) % alignof(std::max_align_t), 0u);
}

TEST(BlockLayout, HeaderAlignmentMatchesMaxAlign) {
    EXPECT_EQ(alignof(BlockHeader), alignof(std::max_align_t));
}

TEST(BlockLayout, HeaderSizeIsAtLeastOnePointerWidth) {
    // The header must be big enough that, once a block is freed, its trailing
    // bytes can hold FreeListLinks (next/prev).
    EXPECT_GE(sizeof(BlockHeader), sizeof(FreeListLinks));
}

// ---------------------------------------------------------------------------
// 2. size / is_free bit-packing round-trips correctly.
// ---------------------------------------------------------------------------

class BlockHeaderPackingTest : public ::testing::TestWithParam<std::size_t> {};

TEST_P(BlockHeaderPackingTest, SizeRoundTripsWhenFree) {
    const std::size_t size = GetParam();
    BlockHeader header(size, /*free=*/true);

    EXPECT_EQ(header.get_size(), size);
    EXPECT_TRUE(header.is_free());
}

TEST_P(BlockHeaderPackingTest, SizeRoundTripsWhenAllocated) {
    const std::size_t size = GetParam();
    BlockHeader header(size, /*free=*/false);

    EXPECT_EQ(header.get_size(), size);
    EXPECT_FALSE(header.is_free());
}

TEST_P(BlockHeaderPackingTest, TogglingFreeDoesNotDisturbSize) {
    const std::size_t size = GetParam();
    BlockHeader header(size, /*free=*/true);

    header.set_free(false);
    EXPECT_EQ(header.get_size(), size);
    EXPECT_FALSE(header.is_free());

    header.set_free(true);
    EXPECT_EQ(header.get_size(), size);
    EXPECT_TRUE(header.is_free());
}

TEST_P(BlockHeaderPackingTest, SettingSizeDoesNotDisturbFreeFlag) {
    const std::size_t size = GetParam();
    BlockHeader header(size, /*free=*/true);

    header.set_size(size + 16);
    EXPECT_EQ(header.get_size(), size + 16);
    EXPECT_TRUE(header.is_free());

    header.set_free(false);
    header.set_size(size + 32);
    EXPECT_EQ(header.get_size(), size + 32);
    EXPECT_FALSE(header.is_free());
}

INSTANTIATE_TEST_SUITE_P(RepresentativeSizes, BlockHeaderPackingTest,
                          ::testing::Values(std::size_t{16}, std::size_t{32}, std::size_t{48},
                                             std::size_t{64}, std::size_t{128}, std::size_t{4096},
                                             std::size_t{1} << 20));

// ---------------------------------------------------------------------------
// 3. Payload address alignment given a fake header at an aligned address.
// ---------------------------------------------------------------------------

TEST(BlockLayout, PayloadAddressIsAligned) {
    // Backing storage aligned to alignof(std::max_align_t), large enough for
    // a header plus a small payload region.
    alignas(alignof(std::max_align_t)) std::array<std::byte, sizeof(BlockHeader) + 256> storage{};

    auto* header = new (storage.data()) BlockHeader(256, /*free=*/false);

    const auto payload_addr = reinterpret_cast<std::uintptr_t>(header->payload());
    EXPECT_EQ(payload_addr % alignof(std::max_align_t), 0u);

    // And it must land exactly sizeof(BlockHeader) bytes after the header.
    const auto header_addr = reinterpret_cast<std::uintptr_t>(header);
    EXPECT_EQ(payload_addr - header_addr, sizeof(BlockHeader));

    header->~BlockHeader();
}

TEST(BlockLayout, PayloadAddressIsAlignedAcrossMultipleAlignedBases) {
    // Several independently-aligned fake header locations, to make sure
    // alignment isn't accidentally satisfied by one lucky address.
    for (int i = 0; i < 8; ++i) {
        alignas(alignof(std::max_align_t)) std::array<std::byte, sizeof(BlockHeader) + 64> storage{};
        // free=false: payload() now asserts !is_free() (see block.hpp), and
        // this test specifically exercises payload(), not free_list_links().
        auto* header = new (storage.data()) BlockHeader(64, /*free=*/false);

        const auto payload_addr = reinterpret_cast<std::uintptr_t>(header->payload());
        EXPECT_EQ(payload_addr % alignof(std::max_align_t), 0u)
            << "iteration " << i << " produced a misaligned payload address";

        header->~BlockHeader();
    }
}

// ---------------------------------------------------------------------------
// BlockFooter sanity: it exists and stores a size_t, independent of any
// wiring into allocation logic (that's Phase 4).
// ---------------------------------------------------------------------------

TEST(BlockLayout, FooterStoresSize) {
    BlockFooter footer{128};
    EXPECT_EQ(footer.size, 128u);
}
