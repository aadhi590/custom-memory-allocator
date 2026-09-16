#include "memory_pool.hpp"

namespace allocator {

std::size_t size_class_for(std::size_t requested_payload_size) noexcept {
    for (std::size_t size_class : kSizeClasses) {
        if (requested_payload_size <= size_class) {
            return size_class;
        }
    }
    return kNoSizeClass;
}

} // namespace allocator
