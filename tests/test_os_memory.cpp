#include "os_memory.hpp"

#include <cstddef>
#include <cstring>
#include <unistd.h>

#include <gtest/gtest.h>

using allocator::os_acquire;
using allocator::os_release;

namespace {

std::size_t page_size() {
    return static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
}

} // namespace

TEST(OsMemory, AcquireReturnsUsableFullyWritableRegion) {
    auto region = os_acquire(4096);
    ASSERT_TRUE(region.has_value());
    EXPECT_NE(region->base, nullptr);
    EXPECT_GE(region->size, 4096u);

    // Write to and read back the full extent of the region to prove every
    // byte the caller was told is usable actually is.
    std::memset(region->base, 0x5A, region->size);
    const auto* bytes = static_cast<const unsigned char*>(region->base);
    for (std::size_t i = 0; i < region->size; ++i) {
        ASSERT_EQ(bytes[i], 0x5A) << "byte " << i;
    }

    EXPECT_TRUE(os_release(region->base, region->size));
}

TEST(OsMemory, RegionSizeIsAlwaysAWholePageMultiple) {
    auto region = os_acquire(4096);
    ASSERT_TRUE(region.has_value());
    EXPECT_EQ(region->size % page_size(), 0u);

    EXPECT_TRUE(os_release(region->base, region->size));
}

TEST(OsMemory, SmallRequestStillGetsAFullPage) {
    auto region = os_acquire(1);
    ASSERT_TRUE(region.has_value());
    EXPECT_EQ(region->size, page_size());

    auto* bytes = static_cast<unsigned char*>(region->base);
    bytes[0] = 0x7;
    bytes[region->size - 1] = 0x9;
    EXPECT_EQ(bytes[0], 0x7);
    EXPECT_EQ(bytes[region->size - 1], 0x9);

    EXPECT_TRUE(os_release(region->base, region->size));
}

TEST(OsMemory, RequestJustOverOnePageRoundsUpToTwoPages) {
    const std::size_t page = page_size();
    auto region = os_acquire(page + 1);
    ASSERT_TRUE(region.has_value());
    EXPECT_EQ(region->size, page * 2);

    EXPECT_TRUE(os_release(region->base, region->size));
}

// A zero-byte request is rejected rather than silently treated as a 1-page
// minimum -- see the rationale in os_memory.hpp. This test locks in that
// choice as observable behavior.
TEST(OsMemory, ZeroSizeRequestIsRejected) {
    auto region = os_acquire(0);
    EXPECT_FALSE(region.has_value());
}

TEST(OsMemory, MultipleRegionsDoNotOverlap) {
    auto a = os_acquire(4096);
    auto b = os_acquire(4096);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());

    std::memset(a->base, 0x11, a->size);
    std::memset(b->base, 0x22, b->size);

    const auto* a_bytes = static_cast<const unsigned char*>(a->base);
    const auto* b_bytes = static_cast<const unsigned char*>(b->base);
    for (std::size_t i = 0; i < a->size; ++i) {
        ASSERT_EQ(a_bytes[i], 0x11) << "region a byte " << i;
    }
    for (std::size_t i = 0; i < b->size; ++i) {
        ASSERT_EQ(b_bytes[i], 0x22) << "region b byte " << i;
    }

    EXPECT_TRUE(os_release(a->base, a->size));
    EXPECT_TRUE(os_release(b->base, b->size));
}
