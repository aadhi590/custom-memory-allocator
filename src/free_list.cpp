#include "free_list.hpp"

#include <cassert>

namespace allocator {

void FreeList::insert(BlockHeader* block) noexcept {
    assert(block != nullptr);
    assert(block->is_free());

    FreeListLinks* links = block->free_list_links();
    links->prev = nullptr;
    links->next = head_;
    if (head_ != nullptr) {
        head_->free_list_links()->prev = block;
    }
    head_ = block;
}

void FreeList::remove(BlockHeader* block) noexcept {
    assert(block != nullptr);
    assert(block->is_free());

    FreeListLinks* links = block->free_list_links();

    if (links->prev != nullptr) {
        links->prev->free_list_links()->next = links->next;
    } else {
        // block was the head.
        head_ = links->next;
    }

    if (links->next != nullptr) {
        links->next->free_list_links()->prev = links->prev;
    }

    links->next = nullptr;
    links->prev = nullptr;
}

BlockHeader* FreeList::find_first_fit(std::size_t size) const noexcept {
    for (BlockHeader* block = head_; block != nullptr; block = block->free_list_links()->next) {
        if (block->get_size() >= size) {
            return block;
        }
    }
    return nullptr;
}

} // namespace allocator
