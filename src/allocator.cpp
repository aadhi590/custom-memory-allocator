#include "allocator.hpp"

#include <cassert>
#include <cstddef>
#include <new>
#include <optional>
#include <vector>

#include "block.hpp"
#include "free_list.hpp"
#include "os_memory.hpp"

namespace allocator {

namespace {

// Default size of each OS-backed arena requested via os_acquire(). Chosen
// as a middle ground: large enough to amortize the mmap syscall (and its
// page table setup cost) over many small allocations -- one syscall per
// allocation would dominate runtime for small sizes -- but not so large
// that a trivial test or example program reserves an unreasonable amount of
// address space just by calling my_malloc a handful of times. 1 MiB is a
// common arena/chunk size in real allocators for the same reason.
constexpr std::size_t kDefaultArenaSize = 1024 * 1024; // 1 MiB

// The arena size actually used for future arena acquisitions. Defaults to
// kDefaultArenaSize; overridable via set_arena_size_for_testing() so tests
// can force deterministic, small-scale arena rollovers instead of only
// inferring rollover correctness from allocating megabytes of data.
std::size_t& arena_size() noexcept {
    static std::size_t size = kDefaultArenaSize;
    return size;
}

std::size_t align_up(std::size_t size) noexcept {
    constexpr std::size_t alignment = alignof(std::max_align_t);
    return (size + alignment - 1) & ~(alignment - 1);
}

// Minimum total footprint (header + payload + footer) for a chunk of
// memory to stand on its own as a valid free-list block. The payload
// portion must be at least sizeof(FreeListLinks), since a free block's
// payload bytes are reinterpreted as next/prev free-list pointers while
// it sits in the free list (see block.hpp's FreeListLinks overlay) --
// anything smaller couldn't even hold its own list linkage if it were
// freed later.
//
// Used to decide whether splitting a found free block is worthwhile: if
// carving out the requested size would leave a remainder smaller than
// this, the remainder couldn't be a valid standalone block at all (it
// couldn't hold FreeListLinks, and couldn't itself be split further), so
// splitting is skipped and the whole block is handed over instead. This
// trades a little internal fragmentation (the caller gets more than it
// asked for) for guaranteeing every block that ever exists is large
// enough to be usable -- an unsplittable sliver would be memory the
// allocator can never reclaim or hand out again.
constexpr std::size_t kMinBlockSize = sizeof(BlockHeader) + sizeof(FreeListLinks) + sizeof(BlockFooter);

// One OS-backed region the bump allocator carves blocks out of. `used`
// tracks the bump pointer as an offset from `base`; it only ever grows for
// the lifetime of the arena -- freed blocks are returned via the free list
// below, not by rewinding an arena's bump pointer.
struct Arena {
    void* base;
    std::size_t size;
    std::size_t used;
};

// All arenas acquired so far, in acquisition order, so allocator_shutdown()
// knows what to hand back to os_release(). Not thread-safe -- this is a
// single-threaded baseline; a locking or per-thread strategy arrives in
// Phase 8.
std::vector<Arena>& arenas() noexcept {
    static std::vector<Arena> instance;
    return instance;
}

// Blocks freed via my_free(), available for reuse by a future my_malloc()
// call. Not thread-safe, same caveat as arenas() above.
FreeList& free_list() noexcept {
    static FreeList instance;
    return instance;
}

// Returns the arena that has room for `needed` more bytes, requesting a
// fresh one from the OS if the current arena (if any) doesn't have enough
// space left. Returns nullptr if a new arena is needed but the underlying
// OS request fails.
Arena* arena_with_room(std::size_t needed) {
    auto& list = arenas();
    if (!list.empty()) {
        Arena& current = list.back();
        if (current.size - current.used >= needed) {
            return &current;
        }
    }

    // A single allocation larger than the standard arena size still needs
    // to succeed, so request exactly enough for it in that case rather than
    // failing outright.
    const std::size_t request_size = needed > arena_size() ? needed : arena_size();
    std::optional<OsRegion> region = os_acquire(request_size);
    if (!region.has_value()) {
        return nullptr;
    }

    list.push_back(Arena{region->base, region->size, 0});
    return &list.back();
}

// ---------------------------------------------------------------------------
// Arena-boundary detection
//
// block.hpp's next_physical_header()/preceding_footer() helpers are pure
// address arithmetic -- they compute where a neighboring block *would* be,
// but block.hpp has no notion of arenas and so cannot know whether that
// address actually holds a real block. A block's physical neighbor only
// exists if it lies within the SAME arena's *carved* region
// [arena.base, arena.base + arena.used) -- the portion that's actually
// been placement-constructed into real BlockHeaders by bump allocation.
// Anything at or beyond arena.used (even if still within arena.size) is
// unwritten/uncarved space, not a block; anything before arena.base
// belongs to a different mapping (or is unmapped) entirely.
//
// These helpers exist specifically so coalescing (and the arena-boundary
// test in test_coalescing.cpp) never dereferences a computed neighbor
// address without first confirming it's real.
// ---------------------------------------------------------------------------

// Finds the arena that owns `addr`, i.e. addr falls within that arena's
// carved region [arena.base, arena.base + arena.used). Returns nullptr if
// no tracked arena owns it (shouldn't happen for a block this allocator
// itself constructed, but callers still check the result).
const Arena* arena_owning(const void* addr) noexcept {
    const auto* byte_addr = static_cast<const std::byte*>(addr);
    for (const Arena& arena : arenas()) {
        const auto* base = static_cast<const std::byte*>(arena.base);
        if (byte_addr >= base && byte_addr < base + arena.used) {
            return &arena;
        }
    }
    return nullptr;
}

// Returns `block`'s free right neighbor if one exists within the same
// arena, or nullptr if there is no right neighbor (block is the last
// carved block in its arena) or the right neighbor exists but is
// currently allocated.
BlockHeader* right_neighbor_if_free(BlockHeader* block) noexcept {
    const Arena* arena = arena_owning(block);
    assert(arena != nullptr && "block does not belong to any tracked arena");
    assert(block->footer_in_sync() && "block's footer is stale before computing its right neighbor");

    BlockHeader* candidate = block->next_physical_header();
    const auto* candidate_bytes = reinterpret_cast<const std::byte*>(candidate);
    const auto* arena_base = static_cast<const std::byte*>(arena->base);
    const std::byte* carved_end = arena_base + arena->used;

    // The candidate header's own footprint must lie entirely within this
    // arena's carved region before it's safe to dereference at all.
    if (candidate_bytes < arena_base || candidate_bytes + sizeof(BlockHeader) > carved_end) {
        return nullptr;
    }

    if (!candidate->is_free()) {
        return nullptr;
    }

    assert(candidate->footer_in_sync() && "right neighbor's footer is stale");
    return candidate;
}

// Returns `block`'s free left neighbor if one exists within the same
// arena, or nullptr if there is no left neighbor (block is the first
// carved block in its arena) or the left neighbor exists but is currently
// allocated.
BlockHeader* left_neighbor_if_free(BlockHeader* block) noexcept {
    const Arena* arena = arena_owning(block);
    assert(arena != nullptr && "block does not belong to any tracked arena");
    assert(block->footer_in_sync() && "block's footer is stale before computing its left neighbor");

    const auto* arena_base = static_cast<const std::byte*>(arena->base);
    const auto* block_bytes = reinterpret_cast<const std::byte*>(block);

    // Reading the preceding footer requires sizeof(BlockFooter) bytes
    // immediately before this block's header to exist within the arena.
    if (block_bytes - sizeof(BlockFooter) < arena_base) {
        return nullptr;
    }

    BlockFooter* prev_footer = block->preceding_footer();
    BlockHeader* candidate = header_from_footer(prev_footer);
    const auto* candidate_bytes = reinterpret_cast<const std::byte*>(candidate);

    // The address header_from_footer() computed (from the footer's own
    // stored size) must itself land within the arena's carved region and
    // strictly before `block` -- otherwise the footer's stored size was
    // corrupt/stale, and trusting it further would be unsafe.
    if (candidate_bytes < arena_base || candidate_bytes >= block_bytes) {
        return nullptr;
    }

    if (!candidate->is_free()) {
        return nullptr;
    }

    assert(candidate->footer_in_sync() && "left neighbor's footer is stale");
    return candidate;
}

} // namespace

void* my_malloc(std::size_t size) noexcept {
    const std::size_t payload_size = align_up(size);

    // First, try to satisfy the request by reusing a previously-freed
    // block.
    if (BlockHeader* reused = free_list().find_first_fit(payload_size)) {
        free_list().remove(reused);

        const std::size_t reused_size = reused->get_size();
        const std::size_t remainder = reused_size - payload_size;

        if (remainder >= kMinBlockSize) {
            // The leftover after carving out exactly payload_size is big
            // enough to stand on its own as a free block: shrink `reused`
            // to the requested size and carve a new free block out of the
            // remainder, instead of handing the whole (oversized) block
            // over and wasting the excess.
            reused->set_size(payload_size);
            reused->sync_footer();

            BlockHeader* remainder_header = reused->next_physical_header();
            const std::size_t remainder_payload_size = remainder - sizeof(BlockHeader) - sizeof(BlockFooter);
            new (static_cast<void*>(remainder_header)) BlockHeader(remainder_payload_size, /*free=*/true);
            remainder_header->sync_footer();
            free_list().insert(remainder_header);
        }
        // else: the leftover would be smaller than kMinBlockSize -- an
        // unusable sliver that could never hold FreeListLinks or stand on
        // its own as a block. Hand over the whole block instead (accepting
        // a little internal fragmentation); see kMinBlockSize's comment.

        reused->set_free(false);
        return reused->payload();
    }

    // No free block fit; fall back to bump-pointer allocation. Every block
    // now carries a footer as well as a header (Phase 4), so room for both
    // must be reserved up front.
    const std::size_t needed = sizeof(BlockHeader) + payload_size + sizeof(BlockFooter);

    Arena* arena = arena_with_room(needed);
    if (arena == nullptr) {
        return nullptr;
    }

    std::byte* block_addr = static_cast<std::byte*>(arena->base) + arena->used;
    // Placement-construct the header at the bump pointer, marked allocated
    // (not free) since it's being handed out immediately.
    auto* header = new (block_addr) BlockHeader(payload_size, /*free=*/false);
    header->sync_footer();
    arena->used += needed;

    return header->payload();
}

void my_free(void* ptr) noexcept {
    if (ptr == nullptr) {
        return;
    }

    // Recover the BlockHeader from the payload pointer -- the mirror image
    // of how payload() computes the forward direction (header address +
    // sizeof(BlockHeader)).
    auto* header = reinterpret_cast<BlockHeader*>(static_cast<std::byte*>(ptr) - sizeof(BlockHeader));

    header->set_free(true);

    // Coalesce with physical neighbors, if any are free. `merged` tracks
    // whichever header currently represents the (possibly-growing) merged
    // block; it starts as the block we just freed.
    BlockHeader* merged = header;

    // Coalesce right first, then coalesce the (possibly-already-grown)
    // result with left -- this way there's only ever one merge operation
    // in flight at a time, instead of juggling three blocks (left,
    // this one, right) simultaneously when both neighbors are free.
    if (BlockHeader* right = right_neighbor_if_free(merged)) {
        free_list().remove(right);

        // right's old header/payload bytes become part of merged's
        // payload; right's old footer position becomes merged's new
        // footer (see docs/memory-model.md for the worked-out arithmetic).
        // No destruction needed -- BlockHeader is a trivial type, and
        // right was already unlinked from the free list above.
        const std::size_t combined_size = merged->get_size() + sizeof(BlockFooter) + sizeof(BlockHeader) + right->get_size();
        merged->set_size(combined_size);
        merged->sync_footer();
    }

    if (BlockHeader* left = left_neighbor_if_free(merged)) {
        free_list().remove(left);

        // left absorbs merged: left's header survives as the merged
        // block's header, so its footer must reflect the new combined
        // size (merged's own header/footer bytes become interior payload).
        const std::size_t combined_size = left->get_size() + sizeof(BlockFooter) + sizeof(BlockHeader) + merged->get_size();
        left->set_size(combined_size);
        left->sync_footer();
        merged = left;
    }

    // Exactly one insert for whatever `merged` ended up being -- the
    // originally-freed block, that block grown to the right, grown to the
    // left, or grown on both sides. Neighbors that were absorbed were
    // already remove()'d above and are never separately inserted.
    free_list().insert(merged);
}

void allocator_shutdown() noexcept {
    // Releases every arena acquired via my_malloc() back to the OS. Exists
    // so tests/examples can run repeatedly within one process without
    // leaking OS memory; a real caller embedding this allocator has no
    // reason to call this under normal operation, since the OS reclaims
    // everything at process exit regardless.
    for (const Arena& arena : arenas()) {
        [[maybe_unused]] const bool released = os_release(arena.base, arena.size);
    }
    arenas().clear();

    // The free list holds BlockHeader* pointers into the arenas just
    // released -- those blocks no longer exist, so the list must be reset
    // rather than left pointing at now-unmapped memory.
    free_list() = FreeList{};
}

void set_arena_size_for_testing(std::size_t size) noexcept {
    arena_size() = size;
}

void reset_arena_size_for_testing() noexcept {
    arena_size() = kDefaultArenaSize;
}

std::size_t arena_count_for_testing() noexcept {
    return arenas().size();
}

bool right_neighbor_is_free_for_testing(void* ptr) noexcept {
    auto* header = reinterpret_cast<BlockHeader*>(static_cast<std::byte*>(ptr) - sizeof(BlockHeader));
    return right_neighbor_if_free(header) != nullptr;
}

bool left_neighbor_is_free_for_testing(void* ptr) noexcept {
    auto* header = reinterpret_cast<BlockHeader*>(static_cast<std::byte*>(ptr) - sizeof(BlockHeader));
    return left_neighbor_if_free(header) != nullptr;
}

std::size_t free_list_size_for_testing() noexcept {
    return free_list().size();
}

} // namespace allocator
