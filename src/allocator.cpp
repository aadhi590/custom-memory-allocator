#include "allocator.hpp"

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

} // namespace

void* my_malloc(std::size_t size) noexcept {
    const std::size_t payload_size = align_up(size);

    // First, try to satisfy the request by reusing a previously-freed
    // block. This phase does not split an oversized free block down to the
    // requested size -- a free block that's larger than needed is handed
    // over whole, wasting the remainder until Phase 4 adds splitting. This
    // is a deliberate, documented limitation, not a bug: it's the simplest
    // correct reuse policy, and it's what a whole-block-only free list can
    // offer before splitting exists.
    if (BlockHeader* reused = free_list().find_first_fit(payload_size)) {
        free_list().remove(reused);
        reused->set_free(false);
        return reused->payload();
    }

    // No free block fit; fall back to bump-pointer allocation.
    const std::size_t needed = sizeof(BlockHeader) + payload_size;

    Arena* arena = arena_with_room(needed);
    if (arena == nullptr) {
        return nullptr;
    }

    std::byte* block_addr = static_cast<std::byte*>(arena->base) + arena->used;
    // Placement-construct the header at the bump pointer, marked allocated
    // (not free) since it's being handed out immediately.
    auto* header = new (block_addr) BlockHeader(payload_size, /*free=*/false);
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
    free_list().insert(header);
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

} // namespace allocator
