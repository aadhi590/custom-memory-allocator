# Benchmarks

No benchmarks exist yet. This document will be filled in starting Phase 9,
once the allocator has enough functionality (splitting, coalescing,
thread-caching) for throughput/fragmentation numbers to be meaningful.

Planned contents once populated:
- Methodology (workload generators, comparison baseline against glibc
  `malloc`/`free`, hardware/OS the numbers were captured on).
- Throughput (allocations/sec) across size classes.
- Fragmentation under sustained alloc/free churn.
- Multi-threaded scaling once the thread-cache layer (Phase 8) exists.
