# Architecture

This document describes the layered architecture of `custom-memory-allocator`,
a from-scratch general-purpose heap allocator for C++17 on Linux (x86-64). It
assumes no prior familiarity with the project.

## Why a layered design

A memory allocator has to satisfy two conflicting goals at once: it must be
*fast* on the hot path (most `malloc`/`free` calls should be a handful of
instructions), and it must be *correct* under adversarial usage patterns
(fragmentation, wildly varying sizes, concurrent threads). Splitting the
allocator into layers, each with one job, keeps those concerns from bleeding
into each other. Each layer only talks to the one directly below it, which
means:

- The bump/free-list logic doesn't need to know how memory was obtained from
  the OS.
- The OS-memory layer doesn't need to know anything about block headers,
  splitting, or coalescing.
- A per-thread caching layer (added much later) can be inserted above the
  allocation manager without touching anything below it.

## Layers

```
┌─────────────────────────────────────────────────────────────────┐
│  Application                                                     │
│  (calls new/delete, malloc/free, or the allocator API directly)  │
└───────────────────────────────┬───────────────────────────────────┘
                                 │
┌───────────────────────────────▼───────────────────────────────────┐
│  Allocator API                                                     │
│  my_malloc(size) / my_free(ptr) / my_realloc(ptr, size)            │
│  include/allocator.hpp, src/allocator.cpp                          │
│  - the only layer application code is meant to call directly       │
│  - validates arguments, delegates to the allocation manager        │
└───────────────────────────────┬───────────────────────────────────┘
                                 │
┌───────────────────────────────▼───────────────────────────────────┐
│  Allocation Manager                                                 │
│  (part of allocator.cpp's internals in early phases; may split      │
│  into its own translation unit once it grows)                       │
│  - decides which free list / size class to search                   │
│  - drives splitting a block that's larger than requested             │
│  - drives coalescing adjacent free blocks on free()                  │
│  - decides when to ask the memory-pool layer for more OS memory      │
└───────────────────────────────┬───────────────────────────────────┘
                                 │
┌───────────────────────────────▼───────────────────────────────────┐
│  Free Lists / Memory Pools                                          │
│  include/free_list.hpp, include/memory_pool.hpp                     │
│  - free_list: intrusive doubly-linked list of free blocks, searched   │
│    first-fit (see note below the diagram)                            │
│  - memory_pool: tracks the OS-backed regions ("arenas") the           │
│    allocator owns, and hands out fresh space when no free block        │
│    is big enough                                                      │
└───────────────────────────────┬───────────────────────────────────┘
                                 │
┌───────────────────────────────▼───────────────────────────────────┐
│  Memory Blocks                                                      │
│  include/block.hpp                                                  │
│  - BlockHeader: per-block metadata (size, is_free, free-list          │
│    linkage overlay) prefixing every block, allocated or free          │
│  - BlockFooter: boundary tag for O(1) backward-neighbor lookup        │
│    during coalescing                                                  │
│  - pure data layout; no allocation policy lives here                  │
└───────────────────────────────┬───────────────────────────────────┘
                                 │
┌───────────────────────────────▼───────────────────────────────────┐
│  OS Memory (mmap-backed)                                            │
│  include/os_memory.hpp, src/os_memory.cpp                           │
│  - thin wrapper around mmap(2)/munmap(2)                             │
│  - the only layer that talks to the kernel                           │
└─────────────────────────────────────────────────────────────────┘
```

### Free list search strategy: first-fit

The general free list (`include/free_list.hpp`) is searched **first-fit**:
walk the list from the head and take the first block whose size is large
enough, rather than **best-fit** (scanning the whole list to find the
smallest block that still fits). First-fit is the standard starting point
for a general-purpose allocator's free list -- it's simpler to implement
correctly, and it avoids paying for a full list walk on every single
allocation. Best-fit's appeal is reduced wasted space per allocation (it
hands out a more tightly-sized block instead of an oversized one), but that
benefit is a real measurement question -- how much fragmentation first-fit
actually costs in practice depends on allocation patterns this project
doesn't have real data for yet. Revisiting first-fit vs. best-fit (or a
hybrid, like next-fit) is deferred until there's a benchmarking harness
(Phase 9) to measure the tradeoff instead of guessing at it.

## Request flow (target shape, once later phases land)

```
 my_malloc(size)
       │
       ▼
 round size up to alignment, add header overhead
       │
       ▼
 ┌─────────────────┐   found a big-enough free block   ┌──────────────────┐
 │ search free list ├───────────────────────────────────► split if needed,  │
 │ for a fit        │                                    │ mark allocated,   │
 └─────────┬────────┘                                    │ return payload    │
           │ no fit found                                └──────────────────┘
           ▼
 ┌─────────────────────┐
 │ ask memory_pool for  │
 │ more OS memory        │
 │ (mmap via os_memory)  │
 └─────────┬─────────────┘
           ▼
 carve a new block, mark allocated, return payload
```

```
 my_free(ptr)
       │
       ▼
 recover BlockHeader from ptr (ptr - sizeof(BlockHeader))
       │
       ▼
 mark block free, insert into free list
       │
       ▼
 check physical neighbors via boundary tags; coalesce if free
```

## Cross-cutting concerns (later phases)

- **Thread safety** (`include/thread_cache.hpp`): a per-thread cache sits
  above the allocation manager so most allocations never touch a shared lock.
- **Statistics** (`include/allocator_stats.hpp`): instrumentation hooks
  threaded through the allocation manager to track bytes in use, allocation
  counts, fragmentation, etc., without the lower layers knowing they're being
  observed.

## Current status

As of Phase 1, only the **Memory Blocks** layer exists (`include/block.hpp`),
with no allocation policy wired up yet. Every other layer is a stub file
documenting where its logic will live. See the [README](../README.md)
roadmap for the full phase breakdown.
