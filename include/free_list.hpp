#pragma once

#include <cstddef>

#include "block.hpp"

// ---------------------------------------------------------------------------
// FreeList
//
// A single, unsegregated (not yet split by size class -- that's Phase 6)
// doubly-linked list of free blocks. Blocks are linked via the
// FreeListLinks (next/prev) overlay defined in block.hpp, which lives in
// the same bytes that become the caller's payload once a block is
// reallocated, so the free list costs no memory beyond the BlockHeader
// itself.
// ---------------------------------------------------------------------------

namespace allocator {

class FreeList {
public:
    FreeList() noexcept = default;

    // Pushes `block` onto the front of the list. `block` must already be
    // marked free (block->is_free()) before calling this -- insert() does
    // not change that flag itself, it only threads the block into the
    // list. Head insertion is O(1); ordering by address isn't required
    // until coalescing (Phase 4) needs it.
    void insert(BlockHeader* block) noexcept;

    // Unlinks `block` from the list. O(1): only touches block's own links
    // and its immediate neighbors, never walks the list looking for it.
    // `block` must currently be linked into this list.
    void remove(BlockHeader* block) noexcept;

    // Walks the list from the head and returns the first block whose size
    // is >= `size`, or nullptr if none fits. First-fit, not best-fit --
    // see docs/architecture.md for why first-fit is the documented choice
    // for this general free list. Does not remove the returned block from
    // the list; callers that intend to hand it out must call remove()
    // themselves.
    [[nodiscard]] BlockHeader* find_first_fit(std::size_t size) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

private:
    BlockHeader* head_ = nullptr;
};

} // namespace allocator
