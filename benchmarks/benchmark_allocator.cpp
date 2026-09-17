// Hand-rolled std::chrono-based benchmark harness -- no external benchmark
// library. Compares system malloc/free, this allocator single-threaded,
// and this allocator's multithreaded throughput across concurrency
// stages. Each stage is a SEPARATE executable (benchmark_allocator_stageN,
// see CMakeLists.txt), all built from this exact same source file but
// linked against src/allocator.cpp compiled with a different
// -DALLOCATOR_CONCURRENCY_STAGE=N -- see src/allocator.cpp's "Concurrency
// staging" comment for why a compile-time switch was chosen over a
// runtime one.
//
// Numbers from actually running this are written into docs/benchmarks.md
// -- nothing in that file is fabricated or estimated.

#include "allocator.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double ns_between(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::nano>(end - start).count();
}

// Per-thread sink for whatever a benchmarked call returns, so the
// compiler can never prove a malloc/free call's result is unused and
// eliminate it -- thread_local (rather than one shared `volatile`
// variable) so writing to it during a multithreaded benchmark never
// itself becomes a point of cross-thread contention that would skew the
// very throughput numbers being measured.
thread_local volatile void* t_sink = nullptr;

// Sizes spanning multiple pooled size classes plus the general path
// (anything > 4096 -- see include/memory_pool.hpp), used identically
// across every benchmark so results are comparable to each other.
constexpr std::size_t kMixedSizes[] = {16, 64, 256, 1024, 4096, 5000, 16384};
constexpr std::size_t kMixedSizeCount = sizeof(kMixedSizes) / sizeof(kMixedSizes[0]);

struct LatencyStats {
    double mean_ns;
    double p99_ns;
};

LatencyStats summarize(std::vector<double>& samples_ns) {
    std::sort(samples_ns.begin(), samples_ns.end());
    const double sum = std::accumulate(samples_ns.begin(), samples_ns.end(), 0.0);
    const double mean = sum / static_cast<double>(samples_ns.size());
    std::size_t p99_index = static_cast<std::size_t>(static_cast<double>(samples_ns.size()) * 0.99);
    if (p99_index >= samples_ns.size()) {
        p99_index = samples_ns.size() - 1;
    }
    return {mean, samples_ns[p99_index]};
}

void print_latency_row(const char* label, const char* op, const LatencyStats& stats) {
    std::printf("%-28s %-6s mean=%10.1f ns   p99=%10.1f ns\n", label, op, stats.mean_ns, stats.p99_ns);
}

// Measures malloc and free latency SEPARATELY: every allocation is timed
// individually and its result stored, THEN every deallocation is timed
// individually in a second pass -- interleaving the two in one timed loop
// would conflate malloc latency with free latency instead of reporting
// each on its own, which is what was asked for.
template <typename MallocFn, typename FreeFn>
void bench_single_threaded_latency(const char* label, MallocFn malloc_fn, FreeFn free_fn, int iterations) {
    std::vector<void*> ptrs(static_cast<std::size_t>(iterations));
    std::vector<double> malloc_samples(static_cast<std::size_t>(iterations));
    std::vector<double> free_samples(static_cast<std::size_t>(iterations));

    for (int i = 0; i < iterations; ++i) {
        const std::size_t size = kMixedSizes[static_cast<std::size_t>(i) % kMixedSizeCount];
        const Clock::time_point start = Clock::now();
        void* ptr = malloc_fn(size);
        const Clock::time_point end = Clock::now();
        t_sink = ptr;
        ptrs[static_cast<std::size_t>(i)] = ptr;
        malloc_samples[static_cast<std::size_t>(i)] = ns_between(start, end);
    }

    for (int i = 0; i < iterations; ++i) {
        void* ptr = ptrs[static_cast<std::size_t>(i)];
        const Clock::time_point start = Clock::now();
        free_fn(ptr);
        const Clock::time_point end = Clock::now();
        free_samples[static_cast<std::size_t>(i)] = ns_between(start, end);
    }

    print_latency_row(label, "malloc", summarize(malloc_samples));
    print_latency_row(label, "free", summarize(free_samples));
}

// Runs `thread_count` threads, each doing `iterations_per_thread`
// independent malloc+free cycles of mixed sizes, and reports combined
// throughput in ops/sec (one malloc + one free = 2 ops). This is what
// actually exercises lock contention realistically: every thread is
// doing real, independent allocator work concurrently for the whole
// measured duration, not just briefly overlapping.
double bench_multithreaded_throughput(int thread_count, int iterations_per_thread) {
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(thread_count));

    const Clock::time_point start = Clock::now();
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([t, iterations_per_thread]() {
            for (int i = 0; i < iterations_per_thread; ++i) {
                const std::size_t size = kMixedSizes[static_cast<std::size_t>(t + i) % kMixedSizeCount];
                void* ptr = allocator::my_malloc(size);
                t_sink = ptr;
                allocator::my_free(ptr);
            }
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }
    const Clock::time_point end = Clock::now();

    const double seconds = std::chrono::duration<double>(end - start).count();
    const double total_ops = static_cast<double>(thread_count) * static_cast<double>(iterations_per_thread) * 2.0;
    return total_ops / seconds;
}

} // namespace

int main() {
#if defined(ALLOCATOR_CONCURRENCY_STAGE)
    std::printf("=== custom-memory-allocator benchmark (Stage %d) ===\n\n", ALLOCATOR_CONCURRENCY_STAGE);
#else
    std::printf("=== custom-memory-allocator benchmark (Stage: default) ===\n\n");
#endif

    constexpr int kLatencyIterations = 50000;

    std::printf("-- Single-threaded latency (mixed pooled + general-path sizes) --\n");
    bench_single_threaded_latency(
        "system malloc/free", [](std::size_t size) { return std::malloc(size); },
        [](void* ptr) { std::free(ptr); }, kLatencyIterations);
    bench_single_threaded_latency(
        "this allocator", [](std::size_t size) { return allocator::my_malloc(size); },
        [](void* ptr) { allocator::my_free(ptr); }, kLatencyIterations);
    allocator::allocator_shutdown();
    std::printf("\n");

    std::printf("-- Multithreaded throughput (this allocator, this binary's compiled-in stage) --\n");
    constexpr int kIterationsPerThread = 200000;
    for (int threads : {1, 2, 4, 8}) {
        const double ops_per_sec = bench_multithreaded_throughput(threads, kIterationsPerThread);
        std::printf("threads=%d  ops/sec=%12.0f\n", threads, ops_per_sec);
        allocator::allocator_shutdown();
    }

    return 0;
}
