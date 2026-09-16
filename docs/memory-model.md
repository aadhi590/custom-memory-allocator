# Memory Model

This document explains the block-level memory layout defined in
`include/block.hpp`: the `BlockHeader` struct, the free-list pointer overlay
trick, and the boundary-tag footer concept. It assumes you've read
[architecture.md](architecture.md) for the broader layer picture.

## The block, end to end

Every region of memory the allocator hands out (or holds in reserve on a free
list) is a contiguous "block" laid out like this:

```
 low address                                                    high address
 ┌───────────────────┬───────────────────────────────┬──────────────────────┐
 │   BlockHeader      │   payload  /  free-list links   │   BlockFooter          │
 │  (fixed size, see   │   (size varies; same bytes,      │  (fixed size, see       │
 │   below)             │    two different meanings         │   "Boundary tags")     │
 │                      │    depending on is_free)          │                        │
 └───────────────────┴───────────────────────────────┴──────────────────────┘
       ▲                          ▲
       │                          │
   header address        payload address =
                          header address + sizeof(BlockHeader)
```

- `BlockHeader` is a fixed-size struct that precedes every block, allocated or
  free.
- Immediately after the header comes a variable-length region whose meaning
  depends on `is_free`: it's either free-list bookkeeping or the caller's
  payload (see "The overlay" below).
- At the very end of the block's region, `BlockFooter` mirrors the block's
  size, so a physically-adjacent block can find this block's header by
  reading backward from its own header (see "Boundary tags" below). Every
  block, free or allocated, carries a footer that's kept in sync with its
  header's size whenever that size changes: initial creation, splitting, or
  coalescing (see the worked examples further down).

## `BlockHeader` layout

```cpp
class alignas(alignof(std::max_align_t)) BlockHeader {
    std::size_t size_and_flags_; // size, with is_free packed into bit 0
    // ... accessor methods, no other data members
};
```

`BlockHeader` has exactly one data member. Forcing the class alignment to
`alignof(std::max_align_t)` (16 bytes on x86-64) means the compiler pads
`sizeof(BlockHeader)` up to a multiple of 16 automatically — C++ guarantees
`sizeof(T)` is always a multiple of `alignof(T)`. That's what makes
`payload address = header address + sizeof(BlockHeader)` land on an aligned
address for free, with no extra rounding logic anywhere else in the
allocator. This is checked by `static_assert`s in `block.hpp` and mirrored as
runtime assertions in `tests/test_block_layout.cpp`.

### Bit-packing `size` and `is_free`

Block sizes are always rounded up to a multiple of `alignof(std::max_align_t)`
before being stored, which means the low 4 bits of any valid size are always
zero. `BlockHeader` reclaims the lowest of those bits to store the `is_free`
flag:

```
 size_and_flags_ (63 bits of size, bit 0 = is_free):

  63                                                        1   0
 ┌───────────────────────────────────────────────────────┬───┬───┐
 │                    size (rounded to 16)                  │ 0 │ f │
 └───────────────────────────────────────────────────────┴───┴───┘
```

`get_size()` masks the flag bit off; `set_size()` preserves whatever flag bit
was already set while replacing the size bits; `is_free()`/`set_free()` only
ever touch bit 0. No other code in the allocator is allowed to read or write
`size_and_flags_` directly — see design-decisions.md for why this is packed
rather than kept as a separate `bool`.

## The overlay: free-list pointers vs. payload

The bytes immediately after `BlockHeader` serve two completely different
purposes depending on the block's state:

- **Free**: those bytes hold a `FreeListLinks{ next, prev }` pair so the block
  can sit in an intrusive free list without needing separate list-node
  allocations.
- **Allocated**: those same bytes *are* the payload the caller received from
  `my_malloc`. The allocator no longer has any business interpreting them.

```
 Free block:
 ┌────────────┬────────────────────────┬─────────┬──────────┐
 │ BlockHeader │ next (ptr) │ prev (ptr) │ (unused) │ Footer   │
 └────────────┴────────────────────────┴─────────┴──────────┘

 Same block after allocation:
 ┌────────────┬─────────────────────────────────────────────┐
 │ BlockHeader │            caller's payload bytes              │
 └────────────┴─────────────────────────────────────────────┘
```

This is implemented with a `reinterpret_cast` from the payload pointer to
`FreeListLinks*` (`BlockHeader::free_list_links()`), not a `union`. See
`design-decisions.md`-adjacent commentary in `block.hpp` for the reasoning:
the payload side has no fixed type to name as a union arm (it's raw bytes the
*caller* reinterprets as whatever object they want), so there's nothing valid
to put opposite `FreeListLinks` in a union. The overlay is safe under the
standard-layout / pointer-interconvertibility rules because both "views" are
computed the same way: header address + `sizeof(BlockHeader)`.

**Invariant**: `free_list_links()` is only ever called on a block currently
marked free; `payload()` is only ever handed to a caller for a block
currently marked allocated. This can't be enforced at compile time — it's an
invariant the allocation manager is responsible for upholding, since only it
knows which state a block is in at any given moment — but since Phase 3 it
*is* enforced at runtime: `payload()` asserts `!is_free()` and
`free_list_links()` asserts `is_free()`, both compiled out under `NDEBUG`
like any other `<cassert>` use. See `docs/design-decisions.md`'s "Known
issues / deferred hardening" section for how this was added.

## Boundary tags (`BlockFooter`)

```cpp
struct alignas(alignof(std::max_align_t)) BlockFooter {
    std::size_t size; // mirrors this block's BlockHeader::get_size()
};
```

A `BlockFooter` at the end of a block's region stores a copy of that block's
size. Given a block's header, you can compute where its footer would be
(`header address + sizeof(BlockHeader) + size`, exposed as
`BlockHeader::footer()`); given a block's *footer*, you can equally read
`sizeof(BlockFooter)` bytes just before it to recover that block's size, then
step backward that many bytes to land exactly on the neighbor's header —
without walking the heap from the start (`BlockHeader::preceding_footer()`
plus the free function `header_from_footer()`).

```
 ... [ Header A ][ payload A ][ Footer A ][ Header B ][ payload B ][ Footer B ] ...
                                    ▲             │
                                    └─────────────┘
                     from Header B, step back sizeof(Footer)
                     to read Footer A, then back size_A bytes
                     to land on Header A directly (O(1))
```

Just like `BlockHeader`, `BlockFooter` is forced to
`alignas(alignof(std::max_align_t))`, padding its size from a natural 8 bytes
(one `size_t`) up to 16. This isn't optional bloat: the *next* block's header
sits immediately after this block's footer, and `BlockHeader` itself requires
16-byte alignment. Since a footer's own address is already guaranteed
16-byte-aligned (header address + header size + payload size are all
multiples of 16), the only way the *following* header stays aligned too is if
`sizeof(BlockFooter)` is also a multiple of 16. Left at its natural 8 bytes,
every block after the first in an arena would land 8-byte-but-not-16-byte
aligned — so every block now costs 32 bytes of header+footer overhead
(16 + 16), not 16 + 8.

This is what coalescing uses to answer "is my left neighbor free?" in
constant time, so two (or three) adjacent free blocks can be merged into one
without a linear scan — see the worked example below. `preceding_footer()`
and `next_physical_header()` are pure address arithmetic and never
dereference anything themselves; `block.hpp` has no notion of arenas, so it
can't know whether a computed neighbor address is real. The arena-boundary
check that makes it safe to actually read through those addresses lives in
`allocator.cpp` (`arena_owning()`, `right_neighbor_if_free()`,
`left_neighbor_if_free()`), not here — see that file's comments and
`tests/test_coalescing.cpp`'s arena-boundary tests for the full reasoning.

## Splitting: a worked example

Say a 512-byte free block exists (header + 512-byte payload + footer), and
`my_malloc(100)` is called. The requested size rounds up to
`align_up(100) = 112`. Carving out 112 bytes would leave
`512 - 112 = 400` bytes free — comfortably larger than `kMinBlockSize` (48
bytes: header + a `FreeListLinks`-sized payload + footer), so the block is
split rather than handed over whole:

```
 Before (one free block, payload = 512):

 ┌────────┬──────────────────────────────────────────────────┬────────┐
 │ Header │                  512 free bytes                     │ Footer │
 │  free   │           (FreeListLinks at the front)               │  512   │
 └────────┴──────────────────────────────────────────────────┴────────┘

 After my_malloc(100) splits it:

 ┌────────┬──────────────┬────────┬────────┬────────────────────┬────────┐
 │ Header │  112 alloc'd  │ Footer │ Header │   368 free bytes     │ Footer │
 │ alloc'd │   payload     │  112   │  free   │ (FreeListLinks front) │  368   │
 └────────┴──────────────┴────────┴────────┴────────────────────┴────────┘
    ▲ returned to caller                        ▲ inserted into free list
```

The first block is shrunk in place (`set_size(112)` + `sync_footer()`) and
handed back allocated; the remainder (`400 - 16 - 16 = 368` bytes of usable
payload) becomes a brand-new free block, placement-constructed right after
the shrunk block's new footer (`BlockHeader::next_physical_header()`), and
inserted into the free list. If the leftover would have been smaller than
`kMinBlockSize` instead, splitting is skipped entirely and the whole
oversized block is handed over — an unsplittable sliver would be memory the
allocator could never reclaim or hand out again, so a little internal
fragmentation is accepted instead. See `tests/test_splitting.cpp` for this
scenario (and the no-split/exact-boundary cases) verified exactly.

## Coalescing: a worked example

Say three 64-byte blocks `A`, `B`, `C` were allocated in that order (so
they're physically adjacent, low to high address), then freed as: free `A`,
free `C`, then free `B` — the "both neighbors free" case, the trickiest one
to get right.

```
 All three allocated:

 [ Header A ][ 64 A ][ Footer A ][ Header B ][ 64 B ][ Footer B ][ Header C ][ 64 C ][ Footer C ]
    alloc'd              alloc'd              alloc'd

 After freeing A and C (B still allocated -- two separate free-list entries):

 [ Header A ][ 64 A ][ Footer A ][ Header B ][ 64 B ][ Footer B ][ Header C ][ 64 C ][ Footer C ]
     free                alloc'd               free

 Freeing B coalesces right first (B absorbs C), then left (A absorbs the
 grown B) -- never juggling all three blocks' bookkeeping simultaneously:

 [ Header A ][             256 free bytes              ][ Footer A (256) ]
     free      (FreeListLinks at the front, from A's old payload start)
```

The merged payload (256 bytes) is larger than the three original 64-byte
payloads summed (192) because `B`'s and `C`'s old header/footer bytes (two
pairs, 32 bytes each = 64 bytes total) are no longer boundary tags for
anything -- they became ordinary interior payload bytes once `A`'s header
took over representing the whole region: `192 + 64 = 256`.

Step by step, in the order `my_free(B)` actually performs them:

1. **Coalesce right**: `right_neighbor_if_free(B)` finds `C`, free. `C` is
   `remove()`d from the free list, and `B`'s size grows to
   `64 + sizeof(Footer) + sizeof(Header) + 64 = 160` — `B`'s footer is
   rewritten at exactly the address `C`'s old footer occupied (the arithmetic
   works out so the merged block's new footer position and the absorbed
   block's old footer position are the same bytes; see the `combined_size`
   derivation in `allocator.cpp`'s `my_free()`).
2. **Coalesce left**: `left_neighbor_if_free(B)` (now size 160) finds `A`,
   free. `A` is `remove()`d, and `A`'s size grows to
   `64 + sizeof(Footer) + sizeof(Header) + 160 = 256`. `A`'s header now
   represents the whole merged region; `B`'s and `C`'s old header bytes are
   just interior payload now, never read as headers again.
3. **Insert once**: the final merged block (`A`, size 256) is inserted into
   the free list exactly one time. `B` and `C` were already removed in steps
   1 and 2 and are never separately re-inserted — the bug this order of
   operations is designed to avoid is inserting an already-absorbed block a
   second time, or leaving a stale, separately-reachable entry for it behind.

`tests/test_coalescing.cpp` verifies this exact scenario
(`FreeingBlockWithBothNeighborsFreeMergesAllThree`), plus the right-only and
left-only cases, the neither-neighbor regression check, and a dedicated
"absorbed neighbor is never reinserted" check via
`free_list_size_for_testing()`.
