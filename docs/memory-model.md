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
 │  (fixed size, see   │   (size varies; same bytes,      │  (fixed size, Phase 4  │
 │   below)             │    two different meanings         │   coalescing)          │
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
  reading backward from its own header (see "Boundary tags" below). This
  footer type is defined now but not yet written by any allocation code —
  that lands in Phase 4.

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
currently marked allocated. Nothing in `block.hpp` enforces this at compile
time — it's an invariant the allocation manager (Phase 2+) is responsible for
upholding, since only it knows which state a block is in at any given
moment.

## Boundary tags (`BlockFooter`)

```cpp
struct BlockFooter {
    std::size_t size; // mirrors this block's BlockHeader::get_size()
};
```

A `BlockFooter` at the end of a block's region stores a copy of that block's
size. Given a block's header, you can compute where its footer would be
(`header address + sizeof(BlockHeader) + size`); given a block's *footer*,
you can equally read `sizeof(BlockFooter)` bytes just before it to recover
that block's size, then step backward that many bytes to land exactly on the
neighbor's header — without walking the heap from the start.

```
 ... [ Header A ][ payload A ][ Footer A ][ Header B ][ payload B ][ Footer B ] ...
                                    ▲             │
                                    └─────────────┘
                     from Header B, step back sizeof(Footer)
                     to read Footer A, then back size_A bytes
                     to land on Header A directly (O(1))
```

This is what Phase 4's coalescing will use to answer "is my left neighbor
free?" in constant time, so two (or three) adjacent free blocks can be merged
into one without a linear scan. As of Phase 1, `BlockFooter` is defined but
nothing writes or reads it yet — allocation, splitting, and freeing logic all
come in later phases.
