// Simplified fragmentation-AWARENESS indicator -- explicitly NOT the full
// 9-workload/formal-fragmentation-metrics suite originally envisioned for
// this project (see the "Scope reduction" entry in
// docs/design-decisions.md). Runs a workload that allocates a large
// batch, frees every other one to create holes, then allocates a second
// batch into that fragmented state, and reports peak RSS (VmHWM, Linux-
// specific -- consistent with this project's Linux-only scope)
// alongside the total bytes actually live at that point. The ratio
// between the two is a rough overhead/fragmentation indicator: the
// closer to 1.0, the less memory the allocator is holding beyond what's
// actually in use (page-rounding, unreleased/uncoalesced free blocks,
// pool slots sitting idle in a size class nothing currently needs, etc).
// This is NOT a formal fragmentation metric (it says nothing about
// address-space layout, external vs. internal fragmentation split, or
// how the ratio would trend under a longer-running or different-shaped
// workload) -- it's a single before/after snapshot meant to give a rough,
// honest sense of overhead, not a rigorous characterization.

#include "allocator.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Reads VmHWM (peak resident set size ever reached by this process) from
// /proc/self/status, in bytes. Returns 0 if it couldn't be read.
std::size_t peak_rss_bytes() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            std::istringstream iss(line.substr(6));
            std::size_t kb = 0;
            iss >> kb;
            return kb * 1024;
        }
    }
    return 0;
}

struct LiveAllocation {
    void* ptr;
    std::size_t size;
};

// Sizes mixing several pooled size classes with a couple of general-path
// sizes, so both allocation paths are exercised by the workload.
constexpr std::size_t kSizes[] = {16, 32, 64, 128, 256, 512, 1024, 5000, 9000};
constexpr std::size_t kSizeCount = sizeof(kSizes) / sizeof(kSizes[0]);

} // namespace

int main() {
    std::printf("=== custom-memory-allocator fragmentation-awareness benchmark ===\n\n");
    std::printf("NOTE: simplified overhead/fragmentation INDICATOR, not a formal\n");
    std::printf("fragmentation-metrics suite -- see docs/benchmarks.md.\n\n");

    std::vector<LiveAllocation> live;
    std::size_t live_bytes = 0;

    // Phase 1: allocate a large initial batch.
    constexpr int kInitialCount = 20000;
    for (int i = 0; i < kInitialCount; ++i) {
        const std::size_t size = kSizes[static_cast<std::size_t>(i) % kSizeCount];
        void* ptr = allocator::my_malloc(size);
        if (ptr == nullptr) {
            std::fprintf(stderr, "allocation failed during initial batch (i=%d)\n", i);
            return 1;
        }
        std::memset(ptr, 0xAB, size);
        live.push_back(LiveAllocation{ptr, size});
        live_bytes += size;
    }

    // Phase 2: free every other allocation, deliberately creating holes
    // interspersed with still-live objects, likely to fragment.
    for (std::size_t i = 0; i < live.size(); i += 2) {
        allocator::my_free(live[i].ptr);
        live_bytes -= live[i].size;
        live[i].ptr = nullptr;
    }

    // Phase 3: allocate a second batch, seeing whether the allocator can
    // reuse the holes just created rather than requesting entirely fresh
    // OS memory for all of it.
    constexpr int kSecondCount = 20000;
    for (int i = 0; i < kSecondCount; ++i) {
        const std::size_t size = kSizes[static_cast<std::size_t>(i) % kSizeCount];
        void* ptr = allocator::my_malloc(size);
        if (ptr == nullptr) {
            std::fprintf(stderr, "allocation failed during second batch (i=%d)\n", i);
            return 1;
        }
        std::memset(ptr, 0xCD, size);
        live.push_back(LiveAllocation{ptr, size});
        live_bytes += size;
    }

    const std::size_t peak_rss = peak_rss_bytes();

    std::size_t live_count = 0;
    for (const LiveAllocation& alloc : live) {
        if (alloc.ptr != nullptr) {
            ++live_count;
        }
    }

    std::printf("Live allocations: %zu\n", live_count);
    std::printf("Live bytes (sum of requested sizes): %zu (%.2f MiB)\n", live_bytes,
                static_cast<double>(live_bytes) / (1024.0 * 1024.0));
    if (peak_rss > 0) {
        std::printf("Peak RSS (VmHWM):  %zu (%.2f MiB)\n", peak_rss,
                    static_cast<double>(peak_rss) / (1024.0 * 1024.0));
        std::printf("Overhead ratio (peak RSS / live bytes): %.2fx\n",
                    static_cast<double>(peak_rss) / static_cast<double>(live_bytes));
    } else {
        std::printf("Peak RSS: unavailable (could not read /proc/self/status)\n");
    }

    for (const LiveAllocation& alloc : live) {
        if (alloc.ptr != nullptr) {
            allocator::my_free(alloc.ptr);
        }
    }
    allocator::allocator_shutdown();

    return 0;
}
