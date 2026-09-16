#pragma once

#include <cstddef>
#include <optional>

// ---------------------------------------------------------------------------
// os_memory
//
// The only module in this codebase allowed to call mmap/munmap directly.
// Every other layer (the bump allocator now, the free-list/pool allocator
// later) asks this module for memory instead of touching the kernel
// interface itself. That keeps OS interaction in one place, which makes it
// mockable/injectable in tests later without threading a fake syscall layer
// through the rest of the allocator.
//
// Why mmap over sbrk: see docs/design-decisions.md for the full reasoning.
// In short, mmap regions are independently addressable and independently
// releasable (sbrk can only shrink from the current break, so a later
// allocation can't be individually returned to the OS), and mmap has no
// single shared mutable OS-level cursor to contend over, which matters once
// this allocator becomes multi-threaded.
//
// Why we round up to page granularity: mmap always backs a request with a
// whole number of pages regardless of what size you ask for -- requesting a
// non-page-multiple size wastes the remainder of that last page either way.
// Rather than let that rounding happen invisibly inside the kernel and have
// callers believe they only have `size` usable bytes, os_acquire() rounds
// up explicitly and reports the real usable size back to the caller, so the
// allocator can use every byte it's actually paying for.
// ---------------------------------------------------------------------------

namespace allocator {

// A region of memory obtained from the OS.
struct OsRegion {
    void* base;       // Base address of the region.
    std::size_t size; // Actual size backing the region, rounded up to a
                       // whole number of pages -- always >= the requested size.
};

// Requests a new region of at least `size` bytes from the OS. `size` is
// rounded up to a whole number of pages before the mmap request is made;
// the returned OsRegion::size reflects that rounded, actually-usable size.
//
// A `size` of 0 is rejected (returns std::nullopt) rather than silently
// treated as a 1-page minimum: a zero-byte request is almost certainly a
// caller bug (e.g. forgetting to round a size up before calling), and
// handing back a full page anyway would mask that bug behind what looks
// like a successful call.
//
// Returns std::nullopt if the underlying mmap call fails, so callers can
// distinguish "no memory available" from a legitimate pointer -- this
// function never throws and never returns an ambiguous nullptr.
[[nodiscard]] std::optional<OsRegion> os_acquire(std::size_t size) noexcept;

// Releases a region previously returned by os_acquire() back to the OS.
// `base` and `size` must exactly match the values from the OsRegion that
// was returned (mmap/munmap require the released size to match what was
// mapped). Returns false if the underlying munmap call fails.
[[nodiscard]] bool os_release(void* base, std::size_t size) noexcept;

} // namespace allocator
