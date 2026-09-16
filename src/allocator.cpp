#include "allocator.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <vector>

#include "block.hpp"
#include "free_list.hpp"
#include "memory_pool.hpp"
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

// One Pool per size class, constructed eagerly (not lazily on first use)
// at program start -- constructing a Pool costs nothing beyond a few
// pointer/size_t member initializations (no OS call happens until the
// pool's first allocate() triggers its first expand()), so there's no
// benefit to deferring it, and eager construction means the pool
// identities below (is_known_pool()) are stable and complete from the
// very first my_malloc()/my_free() call -- no "was this pool constructed
// yet" edge case to reason about.
std::array<Pool, kNumSizeClasses>& pools() noexcept {
    static std::array<Pool, kNumSizeClasses> instance{
        Pool(kSizeClasses[0]), Pool(kSizeClasses[1]), Pool(kSizeClasses[2]),
        Pool(kSizeClasses[3]), Pool(kSizeClasses[4]), Pool(kSizeClasses[5]),
        Pool(kSizeClasses[6]), Pool(kSizeClasses[7]), Pool(kSizeClasses[8]),
    };
    return instance;
}

// Finds the pool for an already-computed size class (as returned by
// size_class_for()). O(kNumSizeClasses) -- a fixed, compile-time-constant
// scan, not a function of how many arenas or allocations exist.
Pool* pool_for_size_class(std::size_t size_class) noexcept {
    for (std::size_t i = 0; i < kNumSizeClasses; ++i) {
        if (kSizeClasses[i] == size_class) {
            return &pools()[i];
        }
    }
    return nullptr; // unreachable given a size_class that came from size_class_for()
}

// Returns true if `candidate` is the address of one of this allocator's
// own, statically-known Pool objects. See the pointer-to-pool routing
// entry in docs/design-decisions.md for the full reasoning: this is what
// makes my_free() able to tell "was this a pooled allocation" apart from
// a general-path one in O(kNumSizeClasses) -- a fixed, compile-time
// constant, not a function of how many arenas or allocations exist --
// without needing to touch the general path's BlockHeader format at all.
bool is_known_pool(const Pool* candidate) noexcept {
    for (const Pool& pool : pools()) {
        if (&pool == candidate) {
            return true;
        }
    }
    return false;
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

// ---------------------------------------------------------------------------
// Shared block-size / coalesce / split helpers
//
// Extracted so my_realloc()'s grow-in-place path (Phase 5) can reuse the
// exact same logic my_malloc()'s splitting and my_free()'s coalescing
// already rely on, instead of re-deriving the header/footer arithmetic a
// third time.
// ---------------------------------------------------------------------------

// Recovers the BlockHeader from a payload pointer -- the mirror image of
// how payload() computes the forward direction (header address +
// sizeof(BlockHeader)). Centralizes this pointer arithmetic so it isn't
// repeated at every call site that needs to go from a caller-visible
// pointer back to its owning header.
BlockHeader* header_of(void* ptr) noexcept {
    return reinterpret_cast<BlockHeader*>(static_cast<std::byte*>(ptr) - sizeof(BlockHeader));
}

// The total payload size a merged block would have if `first` (at the
// lower address) absorbed `second` (physically immediately following
// it): their individual payloads, plus the header+footer overhead between
// them that becomes ordinary interior payload once merged. Pure
// computation, no mutation -- safe to call before committing to a merge,
// which is exactly what my_realloc() needs to decide whether growing in
// place would even be large enough before it commits to anything.
std::size_t combined_payload_size(const BlockHeader* first, const BlockHeader* second) noexcept {
    return first->get_size() + sizeof(BlockFooter) + sizeof(BlockHeader) + second->get_size();
}

// Absorbs `right` (already confirmed free via right_neighbor_if_free(),
// and therefore already a free-list member) into `block`: removes `right`
// from the free list and grows `block`'s size/footer to cover both.
// Does not touch `block`'s is_free() flag or insert anything into the
// free list -- callers own that. Shared between my_free()'s
// right-then-left coalescing and my_realloc()'s grow-in-place path.
void absorb_right_neighbor(BlockHeader* block, BlockHeader* right) noexcept {
    free_list().remove(right);
    block->set_size(combined_payload_size(block, right));
    block->sync_footer();
}

// If `header` (whose current size is already known to be >= payload_size)
// has enough leftover beyond payload_size to stand on its own as a free
// block, shrinks `header` to exactly payload_size and carves the leftover
// into a new free block inserted into the free list. If the leftover
// would be smaller than kMinBlockSize, `header` is left at its current
// (larger) size unchanged -- see kMinBlockSize's comment for why an
// unsplittable sliver is worse than a little internal fragmentation.
// Shared between my_malloc()'s free-list-reuse path and my_realloc()'s
// grow-in-place path, both of which can end up holding a block bigger
// than what was actually requested.
void maybe_split(BlockHeader* header, std::size_t payload_size) noexcept {
    assert(header->get_size() >= payload_size);
    const std::size_t remainder = header->get_size() - payload_size;
    if (remainder < kMinBlockSize) {
        return;
    }

    header->set_size(payload_size);
    header->sync_footer();

    BlockHeader* remainder_header = header->next_physical_header();
    const std::size_t remainder_payload_size = remainder - sizeof(BlockHeader) - sizeof(BlockFooter);
    new (static_cast<void*>(remainder_header)) BlockHeader(remainder_payload_size, /*free=*/true);
    remainder_header->sync_footer();
    free_list().insert(remainder_header);
}

} // namespace

void* my_malloc(std::size_t size) noexcept {
    // Small, common sizes route to a fixed-size pool: O(1) allocation,
    // zero search, zero splitting. Anything too large for any size class
    // falls through to the general free-list/arena path below, completely
    // unchanged from Phase 5.
    const std::size_t size_class = size_class_for(size);
    if (size_class != kNoSizeClass) {
        return pool_for_size_class(size_class)->allocate();
    }

    const std::size_t payload_size = align_up(size);

    // First, try to satisfy the request by reusing a previously-freed
    // block. maybe_split() shrinks it and returns the leftover to the
    // free list if the leftover is large enough to be worthwhile;
    // otherwise the whole (oversized) block is handed over as-is.
    if (BlockHeader* reused = free_list().find_first_fit(payload_size)) {
        free_list().remove(reused);
        maybe_split(reused, payload_size);
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

void* my_calloc(std::size_t count, std::size_t size) noexcept {
    // Overflow check: a manual "would count * size exceed SIZE_MAX"
    // division check, rather than a compiler builtin like
    // __builtin_mul_overflow. Chosen because it's portable standard C++
    // (no GCC/Clang-specific dependency for a single use site) and
    // equally exact -- integer division truncates, so
    // count > SIZE_MAX / size is true exactly when count * size would
    // exceed SIZE_MAX, given size != 0. This is the classic calloc
    // integer-overflow vulnerability class: a naive
    // my_malloc(count * size) that lets the multiplication silently wrap
    // around would allocate a tiny buffer while the caller believes they
    // got count * size bytes, and the caller's first write past the real
    // (tiny) allocation becomes a heap overflow. Returning nullptr here is
    // real, defined failure behavior -- not a crash, not UB.
    if (size != 0 && count > SIZE_MAX / size) {
        return nullptr;
    }

    const std::size_t total = count * size;
    void* ptr = my_malloc(total);
    if (ptr == nullptr) {
        return nullptr;
    }

    // Always zero unconditionally. It is tempting to skip this when the
    // memory obviously came from a fresh arena page (mmap's
    // MAP_ANONYMOUS pages are zeroed by the kernel), but that's only true
    // for brand-new bump-carved memory -- NOT for a block reused from the
    // free list. A reused block can carry leftover FreeListLinks
    // next/prev pointer bytes in its first sizeof(FreeListLinks) bytes
    // (written by FreeList::insert() while it sat free) or simply a
    // previous caller's stale payload data further in. Special-casing
    // "skip the memset because this must be zeroed already" is a real,
    // silent bug class: it would work by coincidence for fresh-arena
    // memory and corrupt the calloc contract the very first time the
    // fast path handed back a reused block instead, with no crash to
    // signal it -- just a caller reading garbage where they expected
    // zeros. See the FreshArenaMemoryIsZeroed/ReusedFreedBlockIsZeroed
    // tests in test_allocator.cpp, which specifically force the reused
    // case to prove this.
    std::memset(ptr, 0, total);
    return ptr;
}

void my_free(void* ptr) noexcept {
    if (ptr == nullptr) {
        return;
    }

    // First, check whether ptr came from a pool. Pooled slots always
    // store their owning Pool* directly in the header immediately before
    // the payload (see memory_pool.hpp's PoolSlotHeader) -- read it
    // speculatively and validate it against the small, fixed set of known
    // pool addresses (is_known_pool(), O(kNumSizeClasses)). This can never
    // produce a false positive for a genuine general-path block: a
    // BlockHeader's first bytes store a small size value (at most a few
    // GiB in realistic use), which can never numerically equal one of the
    // few known Pool object addresses (those live in the program's
    // static-storage-duration data, nowhere near small integer sizes).
    auto* pool_header = reinterpret_cast<PoolSlotHeader*>(static_cast<std::byte*>(ptr) - sizeof(PoolSlotHeader));
    if (is_known_pool(pool_header->owning_pool)) {
        pool_header->owning_pool->deallocate(ptr);
        return;
    }

    // Not pooled -- general-path block. Everything below is Phase 3-5's
    // existing coalescing logic, unchanged.
    BlockHeader* header = header_of(ptr);
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
        // right's old header/payload bytes become part of merged's
        // payload; right's old footer position becomes merged's new
        // footer (see docs/memory-model.md for the worked-out arithmetic).
        // No destruction needed -- BlockHeader is a trivial type, and
        // absorb_right_neighbor() already unlinks right from the free list.
        absorb_right_neighbor(merged, right);
    }

    if (BlockHeader* left = left_neighbor_if_free(merged)) {
        free_list().remove(left);

        // left absorbs merged: left's header survives as the merged
        // block's header, so its footer must reflect the new combined
        // size (merged's own header/footer bytes become interior payload).
        // This can't reuse absorb_right_neighbor() as-is: that helper
        // removes its second argument from the free list, but `merged`
        // isn't a free-list member yet at this point (it's only inserted
        // once, at the end of my_free()) -- calling remove() on it here
        // would corrupt the list by unlinking through uninitialized links.
        left->set_size(combined_payload_size(left, merged));
        left->sync_footer();
        merged = left;
    }

    // Exactly one insert for whatever `merged` ended up being -- the
    // originally-freed block, that block grown to the right, grown to the
    // left, or grown on both sides. Neighbors that were absorbed were
    // already remove()'d above and are never separately inserted.
    free_list().insert(merged);
}

void* my_realloc(void* ptr, std::size_t new_size) noexcept {
    if (ptr == nullptr) {
        return my_malloc(new_size);
    }

    // Determine whether `ptr` is pooled or general-path FIRST -- the two
    // have fundamentally different realloc strategies. See the pointer-to-
    // pool routing reasoning in my_free() above.
    auto* pool_header = reinterpret_cast<PoolSlotHeader*>(static_cast<std::byte*>(ptr) - sizeof(PoolSlotHeader));
    if (is_known_pool(pool_header->owning_pool)) {
        const std::size_t old_payload_size = pool_header->owning_pool->slot_payload_size();

        if (new_size <= old_payload_size) {
            // Already fits within this fixed-size slot -- same pointer, no
            // copy. Mirrors the general path's shrink-in-place behavior
            // below, including realloc(ptr, 0)'s "return unchanged"
            // convention (0 <= old_payload_size always holds).
            return ptr;
        }

        // Pool slots are fixed-size with no adjacent free-space concept
        // the way general-path blocks have -- no header/footer boundary
        // tags, no physical-neighbor coalescing, since every slot in a
        // pool is interchangeable rather than physically meaningful to
        // merge with. Growing a pooled allocation is therefore ALWAYS a
        // copy to a new allocation, never an in-place grow, regardless of
        // how close new_size is to the next size class up or to the
        // general path's threshold. This is a deliberate, documented
        // behavior difference from general-path realloc() -- see
        // docs/design-decisions.md.
        void* new_ptr = my_malloc(new_size);
        if (new_ptr == nullptr) {
            return nullptr; // original pooled block left untouched
        }
        std::memcpy(new_ptr, ptr, old_payload_size); // old_payload_size < new_size here
        my_free(ptr);
        return new_ptr;
    }

    // General-path pointer -- Phase 3-5's existing realloc logic, unchanged.
    const std::size_t new_payload_size = align_up(new_size);
    BlockHeader* header = header_of(ptr);
    const std::size_t old_payload_size = header->get_size();

    // realloc(ptr, 0): the C standard leaves this in
    // implementation-defined/deprecated territory (freeing ptr and
    // returning nullptr is one historical convention, but it's ambiguous
    // -- a caller can't tell "freed successfully" apart from "the
    // reallocation failed" from a nullptr return alone). We deliberately
    // do NOT special-case it that way. my_malloc(0) already establishes
    // this allocator's convention: it returns a valid, non-null pointer
    // to a zero-payload block rather than treating size 0 as an error.
    // Stay consistent with that here -- and conveniently, no special
    // case is even needed: align_up(0) == 0, which always satisfies
    // "new_payload_size <= old_payload_size" below (every existing
    // block's size is >= 0), so my_realloc(ptr, 0) naturally falls
    // through to "return ptr unchanged" on its own.
    if (new_payload_size <= old_payload_size) {
        // Already fits (a same-or-smaller request, including 0): no copy,
        // no reallocation. This deliberately does not shrink-and-split
        // the block down to new_payload_size -- it simply keeps the
        // existing block as-is, trading a little internal fragmentation
        // for avoiding an unnecessary copy on every shrink-realloc call.
        return ptr;
    }

    // Growing. Try to grow in place first: if the right physical neighbor
    // (within the same arena) exists, is free, and absorbing it would be
    // large enough, merge it into this block instead of moving the data.
    // Only the right neighbor is checked -- absorbing the left neighbor
    // would move the payload's start address, which would defeat the
    // "same pointer, no copy" point of growing in place at all.
    if (BlockHeader* right = right_neighbor_if_free(header)) {
        if (combined_payload_size(header, right) >= new_payload_size) {
            absorb_right_neighbor(header, right);
            // The combined block may now be larger than actually needed
            // -- split the excess back into the free list rather than
            // silently handing over the whole absorbed neighbor.
            maybe_split(header, new_payload_size);
            return ptr; // same pointer -- no data copy needed
        }
    }

    // Grow-in-place isn't possible -- fall back to allocate + copy + free.
    void* new_ptr = my_malloc(new_size);
    if (new_ptr == nullptr) {
        // Standard realloc() contract: a failed reallocation must leave
        // the original block completely untouched. Do not free ptr, do
        // not modify it.
        return nullptr;
    }

    const std::size_t copy_size = old_payload_size < new_size ? old_payload_size : new_size;
    std::memcpy(new_ptr, ptr, copy_size);
    my_free(ptr);
    return new_ptr;
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

    // Same idea for every pool: release their arenas too, so tests/
    // examples calling allocator_shutdown() get a genuinely clean slate
    // across both allocation paths, not just the general one.
    for (Pool& pool : pools()) {
        pool.release_all_arenas();
    }
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
    return right_neighbor_if_free(header_of(ptr)) != nullptr;
}

bool left_neighbor_is_free_for_testing(void* ptr) noexcept {
    return left_neighbor_if_free(header_of(ptr)) != nullptr;
}

std::size_t free_list_size_for_testing() noexcept {
    return free_list().size();
}

std::size_t block_payload_size_for_testing(void* ptr) noexcept {
    return header_of(ptr)->get_size();
}

} // namespace allocator
