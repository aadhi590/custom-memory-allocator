#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace allocator {

// Forward declaration: FreeListLinks only needs pointer completeness for
// BlockHeader (a pointer's size/representation doesn't depend on whether
// the pointee type is complete), so it can name BlockHeader* here even
// though the full class is defined further down in this file.
class BlockHeader;

// ---------------------------------------------------------------------------
// BlockHeader
//
// Metadata for one heap block. Every block managed by the allocator (free or
// allocated) is prefixed by one of these headers. Layout, low to high address:
//
//   [ BlockHeader ][ user payload / free-list overlay ... ][ BlockFooter ]
//
// Design notes:
//
// 1. size + is_free bit-packing.
//    All block sizes are rounded up to a multiple of alignof(std::max_align_t)
//    (16 bytes on x86-64), so the low 4 bits of `size` are always zero in a
//    valid size value. We reclaim the lowest bit to store the is_free flag
//    instead of spending a separate bool (which, with padding, would cost a
//    full extra alignment unit in this struct). get_size()/set_size() and
//    is_free()/set_free() are the only code allowed to touch the raw field;
//    everything else goes through them so the bit trick stays contained.
//
// 2. Free-list pointer overlay via reinterpret_cast, not a union.
//    A free block needs `next`/`prev` pointers to sit in the intrusive free
//    list. An allocated block needs that same memory to be handed to the
//    caller as usable payload. We do NOT model this as a union of
//    (FreeListLinks, char payload[]) inside BlockHeader, because:
//      - A union member of unbounded/flexible size isn't valid C++ (no
//        flexible array members in standard C++), so the "payload" side
//        can't actually be represented as a union arm.
//      - The payload doesn't have a fixed type at all -- it's just raw bytes
//        the caller reinterprets as whatever object they want. That's a
//        pointer-arithmetic problem, not a "pick one of these known types"
//        problem, which is what union is for.
//    Instead, FreeListLinks describes the layout that a free block's trailing
//    bytes (immediately after BlockHeader) are interpreted as, and callers
//    get there via payload()/reinterpret_cast. This is well-defined under the
//    common-initial-sequence / pointer-interconvertibility rules for
//    standard-layout types, and it matches how the payload pointer itself is
//    already obtained (header address + sizeof(header), reinterpreted). The
//    tradeoff is the caller must never read free_list_links() on an allocated
//    block, and must never treat payload() as valid on a free block -- this
//    is documented here and enforced by construction (only the allocator's
//    internal free-list code ever calls free_list_links()).
//
// 3. Boundary-tag footer.
//    BlockFooter stores a copy of the block's size at the end of the block's
//    region. Given any block, reading `sizeof(BlockFooter)` bytes just before
//    its header lets you jump directly to the *previous* physical block's
//    header in O(1), which Phase 4's coalescing needs to merge adjacent free
//    blocks without walking the whole heap. It is not wired into allocation
//    logic yet -- this phase only defines the type and its relationship to
//    BlockHeader.
// ---------------------------------------------------------------------------

// Layout used to interpret the bytes immediately following a *free* block's
// header. Meaningless (and never read) while the block is allocated -- at
// that point the same bytes are the start of the caller's payload.
//
// next/prev are BlockHeader* rather than void* -- see the "FreeListLinks
// pointer type" entry in docs/design-decisions.md for why this was chosen
// over void* once free-list code (Phase 3) needed to actually dereference
// these pointers.
struct FreeListLinks {
    BlockHeader* next; // Next free block in this free list, or nullptr.
    BlockHeader* prev; // Previous free block in this free list, or nullptr.
};

// Trailing boundary tag. Mirrors the size stored in the block's BlockHeader
// so a neighbor can identify this block's bounds by reading backward from
// its own header. Not yet wired into any allocation/coalescing logic.
struct BlockFooter {
    std::size_t size; // Same value as this block's BlockHeader::get_size().
};

// Forced to alignof(std::max_align_t): C++ guarantees sizeof(T) is always a
// multiple of alignof(T), so this alone is what makes the static_assert
// below on sizeof(BlockHeader) hold, without hand-adding padding members.
class alignas(alignof(std::max_align_t)) BlockHeader {
public:
    // Constructs a header for a block of `size` bytes (must already be
    // alignment-rounded), initially free.
    explicit BlockHeader(std::size_t size, bool free = true) noexcept {
        set_size(size);
        set_free(free);
    }

    // --- size / is_free accessors -------------------------------------
    //
    // `size_and_flags_` packs the block size in all bits except the lowest,
    // which holds is_free. Because valid sizes are always a multiple of
    // alignof(std::max_align_t) (16 on x86-64), bit 0 of a real size is
    // always 0, so OR-ing in the flag never corrupts the size and masking
    // it off always recovers the exact original size.

    [[nodiscard]] std::size_t get_size() const noexcept {
        return size_and_flags_ & ~std::size_t{kFreeFlagMask};
    }

    void set_size(std::size_t size) noexcept {
        // Caller-provided size must not already have the flag bit set;
        // that would mean it wasn't alignment-rounded to begin with.
        size_and_flags_ = (size & ~std::size_t{kFreeFlagMask}) | (size_and_flags_ & kFreeFlagMask);
    }

    [[nodiscard]] bool is_free() const noexcept {
        return (size_and_flags_ & kFreeFlagMask) != 0;
    }

    void set_free(bool free) noexcept {
        if (free) {
            size_and_flags_ |= kFreeFlagMask;
        } else {
            size_and_flags_ &= ~std::size_t{kFreeFlagMask};
        }
    }

    // --- payload / free-list access -------------------------------------

    // Address of the user payload, valid only while the block is allocated.
    // Guarded by an assert (compiled out under NDEBUG) rather than left as
    // documentation alone -- see the "payload()/free_list_links() runtime
    // invariant guards" entry in docs/design-decisions.md.
    [[nodiscard]] void* payload() noexcept {
        assert(!is_free() && "payload() called on a free block");
        return data_address();
    }

    [[nodiscard]] const void* payload() const noexcept {
        assert(!is_free() && "payload() called on a free block");
        return data_address();
    }

    // View of the trailing bytes as free-list links, valid only while the
    // block is free. Overlays the same storage that payload() points to.
    [[nodiscard]] FreeListLinks* free_list_links() noexcept {
        assert(is_free() && "free_list_links() called on an allocated block");
        return reinterpret_cast<FreeListLinks*>(data_address());
    }

    [[nodiscard]] const FreeListLinks* free_list_links() const noexcept {
        assert(is_free() && "free_list_links() called on an allocated block");
        return reinterpret_cast<const FreeListLinks*>(data_address());
    }

private:
    static constexpr std::size_t kFreeFlagMask = 0x1;

    // Address of the bytes immediately following this header -- the shared
    // computation behind both payload() and free_list_links(). Deliberately
    // has no is_free() assertion of its own: payload() and
    // free_list_links() require *opposite* states, so the assertion has to
    // live in each public accessor rather than here, where it would
    // wrongly reject one of the two valid callers every time.
    [[nodiscard]] void* data_address() noexcept {
        return reinterpret_cast<std::byte*>(this) + sizeof(BlockHeader);
    }

    [[nodiscard]] const void* data_address() const noexcept {
        return reinterpret_cast<const std::byte*>(this) + sizeof(BlockHeader);
    }

    // Size (with the is_free flag packed into bit 0). This is the only data
    // member: next/prev free-list pointers and the user payload both live in
    // the memory immediately following this struct, never inside it.
    std::size_t size_and_flags_;
};

// The payload must start alignof(std::max_align_t)-aligned given an aligned
// header address, with no padding logic needed. That only holds if the
// header's own size is itself a multiple of that alignment.
static_assert(sizeof(BlockHeader) % alignof(std::max_align_t) == 0,
              "BlockHeader size must be a multiple of alignof(std::max_align_t) so that "
              "header address + sizeof(header) is automatically aligned");
static_assert(alignof(BlockHeader) == alignof(std::max_align_t),
              "BlockHeader alignment must exactly match alignof(std::max_align_t)");

} // namespace allocator
