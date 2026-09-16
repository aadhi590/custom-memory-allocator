// Minimal demonstration of the Phase 2 bump-pointer allocator: a few
// differently-sized allocations, proven usable by writing through and
// reading back a distinct byte pattern for each, followed by cleanup.

#include <cstdio>
#include <cstring>

#include "allocator.hpp"

namespace {

bool check_pattern(const void* ptr, std::size_t size, unsigned char pattern) {
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] != pattern) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    void* small = allocator::my_malloc(16);
    void* medium = allocator::my_malloc(256);
    void* large = allocator::my_malloc(4096);

    if (small == nullptr || medium == nullptr || large == nullptr) {
        std::fprintf(stderr, "basic_allocation: allocation failed\n");
        return 1;
    }

    std::memset(small, 0xAA, 16);
    std::memset(medium, 0xBB, 256);
    std::memset(large, 0xCC, 4096);

    const bool ok = check_pattern(small, 16, 0xAA) && check_pattern(medium, 256, 0xBB) &&
                     check_pattern(large, 4096, 0xCC);

    std::printf("basic_allocation: %s\n", ok ? "OK" : "FAILED");

    // my_free() is a documented no-op in this phase (Level 1 bump
    // allocator) -- called here to show the call shape real callers will
    // use once Phase 3 makes deallocation real.
    allocator::my_free(small);
    allocator::my_free(medium);
    allocator::my_free(large);

    allocator::allocator_shutdown();

    return ok ? 0 : 1;
}
