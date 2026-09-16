# Design Decisions

A running log of non-obvious engineering decisions in this project, in the
order they were made. Each entry states the decision, the alternatives
considered, and why the chosen option won.

---

## 1. Why `mmap` over `sbrk` for obtaining memory from the OS

**Decision**: `os_memory.hpp`/`.cpp` (Phase 2+) will request memory from the
kernel via `mmap(2)`, not `sbrk(2)` / `brk(2)`.

**Alternatives considered**:
- `sbrk`/`brk`: grow a single contiguous data-segment break pointer.
- `mmap`: request independent, arbitrarily-sized, arbitrarily-freeable
  virtual memory regions.

**Reasoning**:

- **Independent release.** `sbrk` only supports shrinking the heap from its
  *current end* — you can't return the memory backing an allocation in the
  middle of the heap to the OS without also giving back everything after it.
  `mmap`/`munmap` operate on arbitrary regions, so a large allocation (or an
  entire arena, in the pool-based design from Phase 6 onward) can be returned
  to the OS independently of every other allocation. For an allocator meant
  to demonstrate real design tradeoffs, this matters more than raw
  simplicity.
- **No single shared mutable break pointer.** `sbrk` manipulates one global
  program break. Every thread doing an `sbrk`-based low-level allocation is
  contending over the same pointer, and getting concurrent `sbrk` calls right
  is notoriously easy to get wrong. `mmap` calls are independent syscalls
  with no shared mutable OS-level state to serialize on, which fits the
  eventual multi-threaded design (Phase 8) far better.
- **Large-allocation behavior for free.** Because `mmap` regions are
  independently addressed and unmapped, very large allocations can bypass
  the block/free-list machinery entirely later (mirroring what glibc's
  `malloc` does above `mmap_threshold`) without any special-casing at the
  `sbrk`-break level.
- **Linux-only target is explicit.** This project is scoped to Linux x86-64
  (see README), so there's no portability motivation to prefer `sbrk`'s wider
  historical Unix support. `mmap` is the modern, well-documented interface
  Linux allocators are actually built on.
- **Cost accepted**: `mmap` has higher per-call overhead than `sbrk` (it's a
  full syscall that manipulates page tables, versus `sbrk`'s cheaper break
  adjustment) and normally rounds up to page granularity (4 KiB). This is
  fine because the allocator requests memory in large chunks (arenas) and
  then subdivides them internally via the block/free-list layers — the
  syscall cost is amortized across many small allocations, not paid per
  `my_malloc` call.

---

## 2. Why pack `is_free` into the size field's low bit instead of a separate `bool`

**Decision**: `BlockHeader` stores `is_free` as bit 0 of the same `size_t`
that holds the block size, via `get_size()`/`set_size()`/`is_free()`/
`set_free()` helper methods, rather than as a standalone `bool is_free`
member.

**Alternatives considered**:
- A separate `bool is_free` data member alongside `size_t size`.
- The bit-packed single `size_t` (chosen).

**Reasoning**:

- **The low bits are provably free.** Every block size this allocator ever
  stores is rounded up to a multiple of `alignof(std::max_align_t)` (16 bytes
  on x86-64) before being written into a header, as a hard invariant of the
  allocation logic. That means the low 4 bits of any valid stored size are
  always zero — there is no scenario where a legitimate size value needs bit
  0. Reclaiming that bit isn't a hack layered on top of unrelated data; it's
  using bits that are structurally guaranteed to be unused.
- **Header size stays a clean multiple of the alignment.** A separate
  `bool is_free` next to `size_t size` would, after the compiler's natural
  alignment padding, very likely still round up to the same total struct
  size on a typical x86-64 ABI (a `bool` next to an 8-byte `size_t` gets
  padded to another 8 bytes anyway) — so in practice this decision doesn't
  even save space in the *current* single-field header. The real payoff is
  future-proofing: as more fields are added to `BlockHeader` in later phases
  (e.g., an owning-pool identifier, a magic/canary value for corruption
  detection), keeping flags bit-packed into existing fields avoids
  accumulating separate bool members that each cost padding and each need to
  be threaded through every place the header is copied, zeroed, or compared.
- **Every allocator header this project is modeled after does this.** Bit-
  stealing flags into size fields (using the guaranteed-zero low bits from
  alignment) is standard practice in real allocator implementations
  (dlmalloc-family allocators, for instance, pack multiple flag bits into the
  size field of their boundary tags). Implementing it here, correctly and
  with round-trip tests, is part of what makes this a portfolio piece that
  demonstrates real allocator internals rather than a toy.
- **Cost accepted**: raw bit manipulation is less readable than `header.free`
  at the call site, and it's easy to get wrong (forgetting to mask the flag
  bit back out when reading size, or clobbering it when writing size). This
  is mitigated by centralizing all bit manipulation inside
  `get_size()`/`set_size()`/`is_free()`/`set_free()` — no other code in the
  allocator is allowed to touch `size_and_flags_` directly — and by the
  round-trip tests in `tests/test_block_layout.cpp` that exercise the helpers
  across representative sizes and free/allocated states.

---

## Known issues / deferred hardening

Items identified during the Phase 1 → Phase 2 review of `block.hpp`. Items 1
and 2 were resolved in Phase 3, once free-list code existed to make the
questions concrete; items 3 and 4 remain deliberately deferred.

1. **RESOLVED (Phase 3): `FreeListLinks` now uses `BlockHeader*` instead of
   `void*`.** The open question was whether `BlockHeader*` could even be
   named inside `FreeListLinks`, which is defined above `BlockHeader` in
   `block.hpp`. It can: a forward declaration (`class BlockHeader;`) placed
   before `FreeListLinks` is sufficient, because a pointer's size and
   representation don't depend on the pointee type being complete -- only
   *dereferencing* the pointer requires completeness, and nothing does that
   until `free_list.cpp` (which includes the full `block.hpp` and therefore
   sees the complete `BlockHeader` class). `void*` was not an intentional
   design choice; it was simply what Phase 1 wrote before there was any
   free-list code to prove out the alternative. Switching to `BlockHeader*`
   removes a `reinterpret_cast` from every free-list traversal step and lets
   that code call `is_free()`/`get_size()`/etc. directly on list nodes.

2. **RESOLVED (Phase 3): `payload()` and `free_list_links()` now assert
   their invariants.** `payload()` asserts `!is_free()`;
   `free_list_links()` asserts `is_free()`. Both compile out in release
   builds (`NDEBUG`), matching how the rest of the codebase treats
   `<cassert>` assertions -- these are debug-time bug detectors, not
   input validation. Implementing this required splitting out a private,
   assertion-free `data_address()` helper that both accessors call
   internally: `free_list_links()` needs to compute the same address
   `payload()` does, and it needs to do so on a block that is free (i.e.
   exactly the block state where `payload()`'s assertion would fire), so
   the two accessors can no longer share the address computation directly
   without tripping each other's guard.

3. **`set_size()` silently masks off an already-set flag bit instead of
   asserting the precondition.** The comment above `set_size()` states the
   precondition that the caller's size must not already have bit 0 set, but
   the code doesn't check it — it just masks the bit off unconditionally. A
   caller that accidentally passes an odd size (e.g. a size that wasn't
   alignment-rounded) won't get a signal that something upstream is wrong;
   the value is quietly truncated instead. Consider `assert((size & 0x1) ==
   0)` once there's a debug-assertion story for the project generally.

4. **`BlockHeader`'s constructor defaults `free = true`.** A `BlockHeader{size}`
   constructed without an explicit second argument is free by default. It's
   worth reconsidering whether defaulting to *allocated* (`free = false`)
   is the safer failure mode: if calling code ever forgets to pass the
   argument at a callsite that meant to construct an allocated block, the
   current default fails open (the block looks free and reusable) rather
   than failing closed (the block looks allocated and gets left alone until
   someone deliberately frees it). Revisit alongside item 2 above.
