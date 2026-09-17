// Phase 7: concurrency correctness tests. These are specifically meant to
// be run under ThreadSanitizer (-fsanitize=thread) -- zero data races
// reported is the bar, not just "didn't crash." They test the DEFAULT
// concurrency stage (ALLOCATOR_CONCURRENCY_STAGE's default, Stage 3 --
// see src/allocator.cpp's "Concurrency staging" comment), since that's
// what actually ships; the per-stage throughput comparison lives in
// benchmarks/benchmark_allocator.cpp instead.

#include "allocator.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

class ThreadSafetyTest : public ::testing::Test {
protected:
    void TearDown() override { allocator::allocator_shutdown(); }
};

// Sizes spanning multiple pooled size classes plus the general path
// (anything > 4096 -- see memory_pool.hpp), used to mix allocation
// patterns realistically across threads.
constexpr std::size_t kMixedSizes[] = {16, 64, 256, 1024, 4096, 5000, 9000};
constexpr std::size_t kMixedSizeCount = sizeof(kMixedSizes) / sizeof(kMixedSizes[0]);

} // namespace

// Many threads, each doing thousands of tight alloc/write/verify/free
// cycles across a mix of pooled and general-path sizes, all running
// concurrently. This exercises Stage 3's thread-local cache fast path
// (most allocations), its lock-guarded refill/flush path (whenever a
// cache empties or overflows), and the general path's dedicated mutex,
// all under real concurrent pressure from 8 threads at once.
TEST_F(ThreadSafetyTest, ManyThreadsMixedAllocFreeCyclesNoRaces) {
    constexpr int kThreadCount = 8;
    constexpr int kIterationsPerThread = 2000;

    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (int t = 0; t < kThreadCount; ++t) {
        threads.emplace_back([t]() {
            for (int i = 0; i < kIterationsPerThread; ++i) {
                const std::size_t size = kMixedSizes[static_cast<std::size_t>(t + i) % kMixedSizeCount];
                void* ptr = allocator::my_malloc(size);
                ASSERT_NE(ptr, nullptr) << "thread " << t << " iteration " << i << " size " << size;

                const auto pattern = static_cast<unsigned char>(t);
                std::memset(ptr, pattern, size);
                const auto* bytes = static_cast<const unsigned char*>(ptr);
                for (std::size_t b = 0; b < size; ++b) {
                    ASSERT_EQ(bytes[b], pattern) << "thread " << t << " iteration " << i << " byte " << b;
                }

                allocator::my_free(ptr);
            }
        });
    }

    for (std::thread& th : threads) {
        th.join();
    }
}

// Dedicated cross-thread-free test: a producer thread allocates objects
// and hands each pointer to a consumer thread via a real thread-safe
// queue (not a sequential two-phase handoff), which frees them -- while
// both threads run concurrently. Pool slots are tagged by owning_pool
// alone, never by which thread touched them, so this should work
// correctly regardless of which thread originally allocated a given
// slot; this test verifies that's actually true, under TSan.
TEST_F(ThreadSafetyTest, CrossThreadFreeProducerAllocatesConsumerFrees) {
    constexpr int kCount = 4000;

    std::queue<void*> queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    bool producer_done = false;

    std::thread producer([&]() {
        for (int i = 0; i < kCount; ++i) {
            const std::size_t size = kMixedSizes[static_cast<std::size_t>(i) % kMixedSizeCount];
            void* ptr = allocator::my_malloc(size);
            ASSERT_NE(ptr, nullptr) << "iteration " << i << " size " << size;
            std::memset(ptr, static_cast<unsigned char>(i & 0xFF), size);

            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                queue.push(ptr);
            }
            queue_cv.notify_one();
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            producer_done = true;
        }
        queue_cv.notify_one();
    });

    int consumed = 0;
    std::thread consumer([&]() {
        while (consumed < kCount) {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait(lock, [&] { return !queue.empty() || producer_done; });

            if (queue.empty()) {
                continue; // producer_done but nothing left right now -- loop re-checks the predicate
            }

            void* ptr = queue.front();
            queue.pop();
            lock.unlock();

            // Freed by a DIFFERENT thread than the one that allocated it --
            // the scenario this test exists to exercise.
            allocator::my_free(ptr);
            ++consumed;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(consumed, kCount);

    // Allocator must still be in a fully working state afterward.
    void* ptr = allocator::my_malloc(64);
    ASSERT_NE(ptr, nullptr);
    std::memset(ptr, 0x11, 64);
    const auto* bytes = static_cast<const unsigned char*>(ptr);
    for (std::size_t b = 0; b < 64; ++b) {
        EXPECT_EQ(bytes[b], 0x11) << "byte " << b;
    }
    allocator::my_free(ptr);
}

// Deliberately hammers ONE size class (not a mix) from multiple threads
// simultaneously, so every thread contends for the exact same pool --
// exercising the actual lock-contention path (refill/flush under the
// pool's mutex) repeatedly, rather than mostly hitting each thread's own
// lock-free cache the way a mixed-size workload would.
TEST_F(ThreadSafetyTest, HammeringOneSizeClassFromMultipleThreadsExercisesContention) {
    constexpr int kThreadCount = 8;
    constexpr int kIterationsPerThread = 5000;
    constexpr std::size_t kSize = 64; // single, fixed size class

    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (int t = 0; t < kThreadCount; ++t) {
        threads.emplace_back([t]() {
            for (int i = 0; i < kIterationsPerThread; ++i) {
                void* ptr = allocator::my_malloc(kSize);
                ASSERT_NE(ptr, nullptr) << "thread " << t << " iteration " << i;

                const auto pattern = static_cast<unsigned char>(t);
                std::memset(ptr, pattern, kSize);
                const auto* bytes = static_cast<const unsigned char*>(ptr);
                for (std::size_t b = 0; b < kSize; ++b) {
                    ASSERT_EQ(bytes[b], pattern) << "thread " << t << " iteration " << i << " byte " << b;
                }

                allocator::my_free(ptr);
            }
        });
    }

    for (std::thread& th : threads) {
        th.join();
    }
}
