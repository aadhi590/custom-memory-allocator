#include "memory_pool.hpp"

#include <cstddef>
#include <optional>

#include "os_memory.hpp"

namespace allocator {

namespace {

// Number of slots to carve out of each arena a pool acquires. Chosen so
// the "objects handed out per OS request" stays roughly consistent across
// size classes, rather than using one fixed arena BYTE size for every
// class -- a fixed byte size would make the 16-byte pool's arenas hold
// hundreds of times more slots than the 4096-byte pool's for the same
// arena size, which defeats the point of choosing an arena size to
// amortize the mmap syscall cost evenly (see kDefaultArenaSize's comment
// in allocator.cpp for the same amortization argument applied to the
// general path). Scaling the arena's byte size by slot size instead keeps
// that argument working uniformly well across every size class: the
// 4096-byte class's arenas end up close to the general path's 1 MiB
// default (4096 * 256 ~= 1 MiB), while the 16-byte class's arenas stay
// small (16 * 256 = 4 KiB, one page) rather than reserving a
// disproportionate 1 MiB for tiny objects a test program may never fully
// use.
constexpr std::size_t kSlotsPerArena = 256;

} // namespace

Pool::Pool(std::size_t slot_payload_size) noexcept
    : slot_payload_size_(slot_payload_size), slot_total_size_(sizeof(PoolSlotHeader) + slot_payload_size) {}

bool Pool::expand() noexcept {
    const std::size_t request_size = slot_total_size_ * kSlotsPerArena;
    std::optional<OsRegion> region = os_acquire(request_size);
    if (!region.has_value()) {
        return false;
    }

    arenas_.push_back(Arena{region->base, region->size});

    // Carve however many WHOLE slots actually fit in the (possibly
    // page-rounded-up-larger-than-requested) region and chain them all
    // into the free-slot list. Order doesn't matter -- unlike the general
    // path, pool slots never coalesce, so there's no reason to prefer
    // address order.
    const std::size_t slot_count = region->size / slot_total_size_;
    auto* base = static_cast<std::byte*>(region->base);
    for (std::size_t i = 0; i < slot_count; ++i) {
        auto* slot = reinterpret_cast<PoolSlotHeader*>(base + i * slot_total_size_);
        slot->next_free = free_head_;
        free_head_ = slot;
    }

    return true;
}

void* Pool::allocate() noexcept {
    if (free_head_ == nullptr) {
        if (!expand()) {
            return nullptr;
        }
    }

    PoolSlotHeader* slot = free_head_;
    free_head_ = slot->next_free;

    // Overwrite the (now-stale) next_free link with this slot's owning
    // pool -- the same bytes, reinterpreted, now that the slot is
    // allocated rather than free. See PoolSlotHeader's comment.
    slot->owning_pool = this;
    return reinterpret_cast<std::byte*>(slot) + sizeof(PoolSlotHeader);
}

void Pool::deallocate(void* ptr) noexcept {
    auto* slot = reinterpret_cast<PoolSlotHeader*>(static_cast<std::byte*>(ptr) - sizeof(PoolSlotHeader));
    slot->next_free = free_head_;
    free_head_ = slot;
}

Pool::SlotBatch Pool::allocate_batch(std::size_t max_count) noexcept {
    SlotBatch batch;

    while (batch.count < max_count) {
        if (free_head_ == nullptr && !expand()) {
            break; // OS memory exhausted -- return whatever was gathered so far
        }

        PoolSlotHeader* slot = free_head_;
        free_head_ = slot->next_free;

        slot->next_free = nullptr;
        if (batch.tail == nullptr) {
            batch.head = slot;
        } else {
            batch.tail->next_free = slot;
        }
        batch.tail = slot;
        ++batch.count;
    }

    return batch;
}

void Pool::deallocate_batch(PoolSlotHeader* chain_head, PoolSlotHeader* chain_tail) noexcept {
    chain_tail->next_free = free_head_;
    free_head_ = chain_head;
}

void Pool::release_all_arenas() noexcept {
    for (const Arena& arena : arenas_) {
        [[maybe_unused]] const bool released = os_release(arena.base, arena.size);
    }
    arenas_.clear();
    free_head_ = nullptr;
}

std::size_t size_class_for(std::size_t requested_payload_size) noexcept {
    for (std::size_t size_class : kSizeClasses) {
        if (requested_payload_size <= size_class) {
            return size_class;
        }
    }
    return kNoSizeClass;
}

} // namespace allocator
