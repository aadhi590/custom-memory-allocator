# Benchmarks

All numbers below are real measurements taken on this machine by running
`scripts/run_benchmarks.sh`, which builds the project in Release mode
(`-DCMAKE_BUILD_TYPE=Release`) and runs `benchmark_allocator_stage{1,2,3}`
and `benchmark_fragmentation`. Nothing here is estimated or fabricated —
every figure below is copy-pasted from an actual run's stdout. Re-running
the script on different hardware, or even re-running it on this same
machine, will produce different numbers (see "Run-to-run variance" below);
the point of these numbers is to show real, reproducible *shape*, not to
be treated as precise absolute constants.

## Test environment

- WSL2 (Ubuntu) on Windows 11
- CPU: Intel(R) Core(TM) i5-7200U @ 2.50GHz — **2 physical cores, 4 logical
  (hyperthreaded) cores**
- Kernel: `6.18.33.2-microsoft-standard-WSL2`
- Compiler: GCC 13.2.0, `-O3` (CMake `Release` build type)
- Baseline: glibc `malloc`/`free` (the system allocator WSL's Ubuntu ships
  with), which itself has a per-thread `tcache` (glibc >= 2.26) — i.e. the
  "baseline" this project is compared against is not a naive allocator,
  it's a mature, heavily-tuned one.

The fact that this is a 4-*logical*-core (2 physical + hyperthreading), not
a genuinely 8-core, machine matters a lot for interpreting the 8-thread
numbers below, and the fact that it's running inside WSL2 (Hyper-V
virtualization) matters for interpreting syscall-heavy paths like `mmap`.

## The most important chart: throughput vs. thread count, Stage 1 vs. Stage 3

This is the centerpiece of Part B: does moving from a single global mutex
(Stage 1) to per-thread caching (Stage 3) actually pay off under real
concurrent load?

| Threads | Stage 1 (global mutex) ops/sec | Stage 2 (per-pool locks) ops/sec | Stage 3 (thread-local cache) ops/sec |
|--------:|--------------------------------:|-----------------------------------:|----------------------------------------:|
| 1       | 30,133,870                      | 31,733,691                        | 58,675,977                              |
| 2       | 9,636,047                       | 22,867,139                        | 23,239,668                              |
| 4       | 5,134,943                       | 14,034,792                        | 13,036,652                              |
| 8       | 4,381,093                       | 13,151,449                        | 11,321,106                              |

**Why Stage 3 wins, especially at low thread counts:** at 1 thread, Stage 3
does almost double Stage 1/2's throughput (58.7M vs. ~30-32M ops/sec) —
with only one thread running, there's zero lock *contention* in any stage,
but Stage 1 and Stage 2 still pay the cost of a real `std::mutex`
lock/unlock pair on every single `my_malloc`/`my_free` call. Stage 3 pays
that cost only once every 32 allocations (the batch-refill size — see
`docs/design-decisions.md`), amortizing it away almost entirely, so the
1-thread number is close to "malloc is just a few pointer/branch
operations."

**Why Stage 1 collapses hardest under contention:** going from 1 to 2
threads, Stage 1's throughput falls off a cliff (30.1M -> 9.6M, a ~68%
drop) because *every* allocation and free from *every* thread — regardless
of size class — now serializes on the exact same mutex. Stage 2 and Stage
3 both partition that contention across 9 separate pool mutexes (plus one
general-path mutex), so two threads working on different size classes
barely contend with each other at all, which is exactly why Stage 2 only
drops from 31.7M to 22.9M (~28%) over the same transition.

## A surprising result: Stage 3 does *not* clearly beat Stage 2 at 4 and 8 threads

Looking at the same table above: at 4 threads Stage 2 (14.0M) is actually
slightly *ahead* of Stage 3 (13.0M), and at 8 threads the gap is a bit
wider (13.2M vs. 11.3M). This runs against the naive expectation that
"thread-local caching should always win harder as thread count grows,
since it removes more contention." The task explicitly asks for surprising
results to be reported honestly rather than hidden, so here is the best
current explanation, developed but **not** independently verified with a
profiler in this session:

1. **This machine only has 2 physical cores.** At 4 and especially 8
   threads, the benchmark is running far more software threads than
   hardware execution contexts, so both stages are dominated by OS
   scheduling/time-slicing overhead rather than by allocator-internal lock
   contention. Once the bottleneck moves from "waiting on a mutex" to
   "waiting for the OS to give this thread a core," the two stages'
   different locking strategies matter much less, and the picture can
   invert on any given run.
2. **The benchmark's mixed-size workload includes sizes above the pooled
   threshold (5000, 9000 bytes),** which fall onto the general free-list
   path. Per the documented scope limitation (see `docs/design-decisions.md`
   and the Part A task description), thread-local caching is explicitly
   **not** implemented for that path — both Stage 2 and Stage 3 serialize
   general-path allocations on the exact same `general_path_lock()`. So a
   meaningful fraction of every thread's work in this benchmark hits an
   identically-contended lock in both stages, which caps how much Stage 3
   can pull ahead.
3. Stage 3 does real extra bookkeeping Stage 2 doesn't (checking/filling
   the thread-local cache, splicing batches) — cheap, but not free — so on
   a workload/hardware combination where the pool-lock contention that
   caching is meant to avoid isn't the dominant cost, that fixed overhead
   can show up as a small net loss rather than a gain.

The honest summary: thread-local caching's win is clearest and most
consistent at low thread counts and single-threaded latency, where it
removes lock overhead entirely; its advantage at higher thread counts on
*this* hardware is smaller than expected, and on this run even slightly
negative, most likely because of core oversubscription and the
un-cached general-path lock rather than any bug in Stage 3 itself.

## Another expected-but-worth-flagging pattern: throughput falls as thread count rises, for *every* stage

Every column in the table above decreases monotonically from 1 to 8
threads, including Stage 3. This looks counterintuitive for a
"concurrency" benchmark, but is consistent with the same oversubscription
explanation above: this benchmark measures aggregate ops/sec while
*every* thread is hammering the allocator as fast as possible with no
other work, on a machine with only 2 physical cores. Beyond 2 threads, the
CPU itself is the bottleneck (not any lock), so adding more software
threads adds scheduling and cache-eviction overhead without adding real
parallel execution capacity — throughput per thread drops faster than
thread count rises. This is a property of the benchmark and hardware, not
evidence that the locking design is failing to scale; it's exactly the
kind of oversubscription effect the design-decisions doc flags as a risk
of testing an 8-thread scenario on a 4-logical-core machine.

## Single-threaded latency: this allocator vs. system malloc

Mean and p99 latency in nanoseconds/operation, malloc and free measured in
separate passes, across the mixed pooled + general-path size set used
throughout Part B (16, 64, 256, 1024, 4096, 5000, 9000 bytes).

| Binary (concurrency stage) | Allocator     | malloc mean (ns) | malloc p99 (ns) | free mean (ns) | free p99 (ns) |
|-----------------------------|---------------|------------------:|-----------------:|-----------------:|-----------------:|
| Stage 1                    | system malloc | 1726.0             | 34700.0           | 111.2             | 400.0             |
| Stage 1                    | this allocator| 2207.7             | 34600.0           | 112.8             | 400.0             |
| Stage 2                    | system malloc | 1015.7             | 4500.0            | 102.9             | 400.0             |
| Stage 2                    | this allocator| 1222.7             | 5300.0            | 133.4             | 500.0             |
| Stage 3                    | system malloc | 1005.7             | 4500.0            | 97.8              | 400.0             |
| Stage 3                    | this allocator| 1135.3             | 4800.0            | 118.4             | 500.0             |

**Surprising/unfavorable result, reported honestly:** on every single run,
in every stage, this allocator's mean `malloc` latency is higher than
glibc's — by roughly 20% in the Stage 2/3 runs, and more in the noisier
Stage 1 run (see "run-to-run variance" below). `free` latency is much
closer (within ~15-20ns), and often nearly tied. Best-understood reasons,
not fully confirmed via a syscall tracer in this session:

- **glibc's allocator is not a naive baseline.** It has its own per-thread
  `tcache` (comparable in spirit to this project's Stage 3), decades of
  tuning, and size-class/bin structures more elaborate than this
  project's 9 fixed pools. Beating it on raw latency was never a
  realistic bar for a from-scratch student/portfolio allocator; the goal
  of this project was to *build and understand* an allocator, not to
  out-perform glibc.
- **The general free-list path likely triggers more `mmap` calls than
  glibc needs for the same workload.** This project's general path grows
  its arena in fixed 1 MiB chunks (see `docs/design-decisions.md`); the
  benchmark's size mix includes 5000- and 9000-byte allocations, which
  land on that path. A malloc-heavy-then-free-later access pattern against
  a fixed arena size can require the arena to grow (i.e. call `mmap`)
  more often than an allocator like glibc's, which has more sophisticated
  heap-growth and reuse heuristics tuned over many more years. `mmap` is a
  real syscall — a kernel context switch — and is one of the more
  expensive operations either allocator can perform.
- **WSL2 syscalls are not free-of-virtualization-overhead.** WSL2 runs
  Linux inside a lightweight Hyper-V VM; syscalls that have to cross that
  boundary (like `mmap`) are documented to carry extra latency compared to
  bare-metal Linux. Since this difference is most visible exactly on the
  large, general-path sizes that call `mmap` more often, this is
  consistent with (though not proof of) the `mmap`-frequency explanation
  above.
- The p99 numbers for malloc are *very* close to identical between "this
  allocator" and system malloc in every stage (34600 vs 34700 in Stage 1;
  4800 vs 4500 in Stage 2/3) — the difference is concentrated in the mean,
  not the tail, which is consistent with "most calls are fast and
  comparable, but a subset pay a larger, allocator-specific cost" (e.g.
  an occasional `mmap`) rather than "every call is uniformly slower."

## Run-to-run variance

The Stage 1 latency numbers above (system malloc mean 1726ns, this
allocator mean 2207ns) are both roughly 1.7-2x higher than the
corresponding Stage 2/3 numbers, even though Stage 2 and Stage 3's *own*
system-malloc baseline numbers (measured in the same process, same
run) agree closely with each other (1015.7 vs. 1005.7ns). Since the
system-malloc baseline should not depend on which concurrency stage this
allocator was compiled with, the most likely explanation is environmental
noise specific to that run (page cache still warming up in a build
directory right after a fresh Release build, background Windows/WSL
activity) rather than a real effect. This is reported as-is rather than
re-run-until-it-looks-cleaner, per the instruction to report actual
measured numbers honestly; the qualitative conclusions above (Stage 3 >
Stage 1/2 at low thread counts, general-path lock capping Stage 3's edge
at high thread counts) were consistent across multiple runs taken during
this project's development, even though the exact numbers move from run
to run.

## Fragmentation-awareness indicator (simplified)

This is explicitly **not** the full 9-workload/formal-fragmentation-metrics
suite originally envisioned earlier in the project's design docs (see the
scope-reduction entry in `docs/design-decisions.md`) — it's a single
before/after snapshot meant to give a rough, honest sense of overhead.

Workload: allocate 20,000 objects (mixed pooled + general sizes), free
every other one to deliberately create holes, then allocate 20,000 more
into that fragmented state.

```
Live allocations: 30000
Live bytes (sum of requested sizes): 53,434,736 (50.96 MiB)
Peak RSS (VmHWM):                    59,510,784 (56.75 MiB)
Overhead ratio (peak RSS / live bytes): 1.11x
```

A 1.11x ratio means the process's peak resident memory was about 11%
above the sum of bytes actually requested and live at that point — a
reasonable indicator of low overhead for this workload shape, but again:
this says nothing about external-vs-internal fragmentation split, address-
space layout, or how the ratio would trend under a longer-running or
differently-shaped workload. It's one snapshot, not a formal metric.

## Reproducing these numbers

```
bash scripts/run_benchmarks.sh
```

builds `build-release/` in Release mode and runs `benchmark_allocator_stage1`,
`benchmark_allocator_stage2`, `benchmark_allocator_stage3`, and
`benchmark_fragmentation` in sequence, printing exactly the kind of output
tabulated above.
