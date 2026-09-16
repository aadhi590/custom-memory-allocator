#include "os_memory.hpp"

#include <sys/mman.h>
#include <unistd.h>

namespace allocator {

namespace {

std::size_t page_size() noexcept {
    // sysconf is a syscall; cache the result since the page size cannot
    // change for the lifetime of the process.
    static const std::size_t size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    return size;
}

std::size_t round_up_to_page(std::size_t size) noexcept {
    const std::size_t page = page_size();
    return ((size + page - 1) / page) * page;
}

} // namespace

std::optional<OsRegion> os_acquire(std::size_t size) noexcept {
    if (size == 0) {
        return std::nullopt;
    }

    const std::size_t rounded = round_up_to_page(size);

    void* addr = mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        return std::nullopt;
    }

    return OsRegion{addr, rounded};
}

bool os_release(void* base, std::size_t size) noexcept {
    return munmap(base, size) == 0;
}

} // namespace allocator
