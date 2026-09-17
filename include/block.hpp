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
// its own header (see BlockHeader::preceding_footer()/next_physical_header()
// below). Wired into allocation logic from Phase 4 onward: every block,
// free or allocated, has a footer that must be kept in sync with its
// header's size whenever that size changes (construction, splitting,
// coalescing) -- see docs/memory-model.md for the full worked examples.
//
// Forced to alignof(std::max_align_t), exactly like BlockHeader, and for
// the same structural reason plus one more: BlockFooter's address is
// already guaranteed 16-byte aligned (header address + sizeof(header) +
// payload size are all multiples of 16), but the NEXT block's header sits
// at footer address + sizeof(BlockFooter) -- if sizeof(BlockFooter) were
// left at its natural 8 bytes (one size_t, no padding), every subsequent
// block's header in an arena would land 8-byte-but-not-16-byte aligned,
// violating BlockHeader's own alignas(alignof(std::max_align_t))
// requirement. Padding BlockFooter's size up to 16 bytes keeps every
// header in a chain of physically adjacent blocks aligned automatically,
// at the cost of doubling the per-block footer overhead from 8 to 16
// bytes.
struct alignas(alignof(std::max_align_t)) BlockFooter {
    std::size_t size; // Same value as this block's BlockHeader::get_size().
};

static_assert(sizeof(BlockFooter) % alignof(std::max_align_t) == 0,
              "BlockFooter size must be a multiple of alignof(std::max_align_t) so that "
              "the next physical block's header stays aligned");
static_assert(alignof(BlockFooter) == alignof(std::max_align_t),
              "BlockFooter alignment must exactly match alignof(std::max_align_t)");

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

    // --- footer / physical-neighbor access -------------------------------
    //
    // These are pure address arithmetic based on this block's own size --
    // they never dereference a neighboring block's memory, so they are
    // always safe to *call*. They are NOT always safe to *dereference* the
    // result of: a neighbor only exists if it lies within the same arena
    // as this block, and block.hpp has no notion of arenas (that's
    // allocator.cpp's job, via its own arena-bounds bookkeeping). Callers
    // must verify a computed address is within the owning arena's carved
    // region before reading through it -- see the arena-boundary-safety
    // helpers in allocator.cpp and the worked examples in
    // docs/memory-model.md.

    // Address of this block's own footer, computed from this block's own
    // (already-known-valid) size. Always safe to dereference: the footer
    // is part of this block's own footprint, which the allocator
    // guarantees is backed by real memory for any block it constructed.
    [[nodiscard]] BlockFooter* footer() noexcept {
        return reinterpret_cast<BlockFooter*>(reinterpret_cast<std::byte*>(this) + sizeof(BlockHeader) +
                                               get_size());
    }

    [[nodiscard]] const BlockFooter* footer() const noexcept {
        return reinterpret_cast<const BlockFooter*>(reinterpret_cast<const std::byte*>(this) +
                                                      sizeof(BlockHeader) + get_size());
    }

    // Writes this block's current size into its own footer. Must be
    // called every time this block's size changes -- initial construction,
    // splitting, or coalescing -- so the footer never goes stale. An
    // out-of-sync footer is worse than no footer at all: coalescing trusts
    // a neighbor's footer to locate that neighbor's header, so a stale
    // footer would misdirect coalescing into treating unrelated bytes as a
    // block header.
    void sync_footer() noexcept { footer()->size = get_size(); }

    // Debug-only sanity check: true if this block's footer currently
    // mirrors its header's size. Because a footer's stored size and its
    // owning header's stored size are two independently-written values
    // that must always agree by construction, this is a genuine
    // consistency check, not a tautology -- see the assert call sites in
    // allocator.cpp's coalescing logic, which check this on every block
    // about to be trusted/merged rather than assuming footers are always
    // correct.
    [[nodiscard]] bool footer_in_sync() const noexcept { return footer()->size == get_size(); }

    // Address where the PRECEDING physical block's footer would be, if one
    // exists. Pure address arithmetic (this block's own address minus one
    // footer's width) -- does not dereference anything, so it's always
    // safe to call, but the caller must verify this address is within the
    // owning arena's carved region before reading through it.
    [[nodiscard]] BlockFooter* preceding_footer() noexcept {
        return reinterpret_cast<BlockFooter*>(this) - 1;
    }

    [[nodiscard]] const BlockFooter* preceding_footer() const noexcept {
        return reinterpret_cast<const BlockFooter*>(this) - 1;
    }

    // Address where the NEXT physical block's header would begin, if one
    // exists (immediately after this block's own footer). Computed purely
    // from this block's own size, same safety caveat as preceding_footer().
    [[nodiscard]] BlockHeader* next_physical_header() noexcept {
        return reinterpret_cast<BlockHeader*>(reinterpret_cast<std::byte*>(footer()) + sizeof(BlockFooter));
    }

    [[nodiscard]] const BlockHeader* next_physical_header() const noexcept {
        return reinterpret_cast<const BlockHeader*>(reinterpret_cast<const std::byte*>(footer()) +
                                                      sizeof(BlockFooter));
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
    //
    // Default-initialized to 0 (rather than left indeterminate) because
    // the constructor's first call, set_size(size), reads the CURRENT
    // flag bit out of size_and_flags_ before overwriting the size bits --
    // by design, so a size update never disturbs the flag -- which means
    // that read has to see a well-defined value even on a
    // freshly-constructed object, or it's reading indeterminate memory
    // (technically undefined behavior, and something GCC's -O2 correctly
    // flags as -Wmaybe-uninitialized in Release builds even though the
    // FINAL value after the constructor finishes is always deterministic
    // regardless, since the very next call, set_free(), unconditionally
    // overwrites the flag bit either way).
    std::size_t size_and_flags_ = 0;
};

// The payload must start alignof(std::max_align_t)-aligned given an aligned
// header address, with no padding logic needed. That only holds if the
// header's own size is itself a multiple of that alignment.
static_assert(sizeof(BlockHeader) % alignof(std::max_align_t) == 0,
              "BlockHeader size must be a multiple of alignof(std::max_align_t) so that "
              "header address + sizeof(header) is automatically aligned");
static_assert(alignof(BlockHeader) == alignof(std::max_align_t),
              "BlockHeader alignment must exactly match alignof(std::max_align_t)");

// Recovers the BlockHeader* that owns `footer`, using the footer's own
// stored size (header address = footer address - stored size -
// sizeof(BlockHeader)). This is the one place a footer's stored value is
// actually trusted to locate a block, rather than just mirrored into it --
// callers (allocator.cpp's coalescing logic) are expected to sanity-check
// the result with footer_in_sync() before relying on it further, and to
// have already verified the footer itself lies within the owning arena's
// carved region before calling this (block.hpp has no notion of arenas).
[[nodiscard]] inline BlockHeader* header_from_footer(BlockFooter* footer) noexcept {
    return reinterpret_cast<BlockHeader*>(reinterpret_cast<std::byte*>(footer) - footer->size -
                                           sizeof(BlockHeader));
}

[[nodiscard]] inline const BlockHeader* header_from_footer(const BlockFooter* footer) noexcept {
    return reinterpret_cast<const BlockHeader*>(reinterpret_cast<const std::byte*>(footer) - footer->size -
                                                  sizeof(BlockHeader));
}

} // namespace allocator
