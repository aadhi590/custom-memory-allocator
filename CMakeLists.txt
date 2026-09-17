cmake_minimum_required(VERSION 3.16)
project(custom_memory_allocator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to a Debug build when the user doesn't pick one explicitly.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type (Debug or Release)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release")
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Phase 7: std::mutex/std::thread require linking pthread on Linux.
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# GoogleTest via FetchContent
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)
# Keep GoogleTest's own runtime library settings consistent with this project.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

# ---------------------------------------------------------------------------
# allocator_tests
#
# Only compiles test sources with real content (Phase 1: block layout;
# Phase 2: os_memory and the bump allocator; Phase 3: the free list;
# Phase 4: splitting/coalescing; Phase 5: calloc/realloc; Phase 6: memory
# pools/size classes; Phase 7: concurrency). Empty stub test files are
# added to this target as their phases land.
# ---------------------------------------------------------------------------
add_executable(allocator_tests
    tests/test_block_layout.cpp
    tests/test_os_memory.cpp
    tests/test_allocator.cpp
    tests/test_free_list.cpp
    tests/test_splitting.cpp
    tests/test_coalescing.cpp
    tests/test_realloc.cpp
    tests/test_memory_pool.cpp
    tests/test_thread_safety.cpp
    src/os_memory.cpp
    src/allocator.cpp
    src/free_list.cpp
    src/memory_pool.cpp
)
target_include_directories(allocator_tests PRIVATE include)
target_link_libraries(allocator_tests PRIVATE gtest_main Threads::Threads)

include(GoogleTest)
gtest_discover_tests(allocator_tests)

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
add_executable(basic_allocation
    examples/basic_allocation.cpp
    src/os_memory.cpp
    src/allocator.cpp
    src/free_list.cpp
    src/memory_pool.cpp
)
target_include_directories(basic_allocation PRIVATE include)
target_link_libraries(basic_allocation PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# Benchmarks (Phase 7, Part B)
#
# benchmark_allocator is built once PER concurrency stage (see
# src/allocator.cpp's "Concurrency staging" comment) -- three separate
# executables from the exact same benchmark source, each linked against
# allocator.cpp compiled with a different ALLOCATOR_CONCURRENCY_STAGE, so
# Part B's throughput comparison is measuring genuinely different compiled
# code, not one binary branching at runtime. Intended to be built in
# Release mode (see scripts/run_benchmarks.sh) -- Debug's lack of
# optimization would make the latency/throughput numbers meaningless.
# ---------------------------------------------------------------------------
foreach(STAGE 1 2 3)
    add_executable(benchmark_allocator_stage${STAGE}
        benchmarks/benchmark_allocator.cpp
        src/os_memory.cpp
        src/allocator.cpp
        src/free_list.cpp
        src/memory_pool.cpp
    )
    target_include_directories(benchmark_allocator_stage${STAGE} PRIVATE include)
    target_link_libraries(benchmark_allocator_stage${STAGE} PRIVATE Threads::Threads)
    target_compile_definitions(benchmark_allocator_stage${STAGE} PRIVATE ALLOCATOR_CONCURRENCY_STAGE=${STAGE})
endforeach()
