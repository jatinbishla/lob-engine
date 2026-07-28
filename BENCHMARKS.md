# BENCHMARKS.md

## Methodology

**All committed numbers come from a single machine.** Latency percentiles, the
throughput microbenchmarks, and the histogram in the README are meaningless to
compare unless they share one CPU, clock, and toolchain — so the whole benchmark
section is always regenerated together from one host, never mixed across machines.

**Canonical host:** the Windows 11 / GCC 15.2 machine described under
*Measurement boundary* below. All figures on this page were measured there.

**Required regeneration procedure:**

1. Linux x86 (or the canonical Windows host), **Release** build:
   `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`
2. Pin to one core to cut scheduler jitter, then run the bench from the repo root:
   `taskset -c 2 ./build/bench`   (writes `data/latency_samples.csv` + prints percentiles)
3. Regenerate the charts from that CSV:
   `PYTHONPATH=build python python/make_charts.py`
4. Update the README performance table + microbenchmark line from that same run.

The bench aborts (non-zero) if the active timer's **effective resolution exceeds
10 ns** — see below for why.

### Per-operation timing: rdtsc, not `clock_gettime`

`clock_gettime` costs **~20 ns per call even through the vDSO** (no syscall) — a
large fraction of the sub-100-ns operation we're trying to measure, and paid
twice per sample (before and after). Timing `submit()` with it would fold the
instrument's cost into the result. So on x86 the per-operation path reads the
**timestamp counter directly**:

```cpp
_mm_lfence(); t0 = __rdtsc(); submit(...); t1 = __rdtsc(); _mm_lfence();
```

- **lfence serialization** — `rdtsc` is not ordered against surrounding
  instructions on its own; the `lfence` fences stop the CPU from hoisting work
  into or out of the timed region, so `t1 - t0` brackets exactly `submit()`.
- **Overhead subtraction** — the median tick delta of ~10,000 *empty* timed
  regions (same lfence/rdtsc shape, no `submit()`) is measured once and
  subtracted from every sample, so we report the operation, not the counter read.
- **Runtime calibration** — the invariant TSC advances at a fixed rate unrelated
  to core frequency, so ticks-per-nanosecond is calibrated at startup against
  `steady_clock` over ~200 ms rather than assumed. The calibrated frequency and
  the measured overhead are printed at the top of every run.

On non-x86 hosts the bench falls back to `steady_clock` (and, on Apple Silicon,
the resolution guard below stops it). The rdtsc resolution is one tick (sub-ns on
any multi-GHz TSC), so it clears the 10 ns guard easily.

### Apple Silicon cannot measure this path

On Apple Silicon, `std::chrono::steady_clock` is backed by the **mach timebase,
which ticks at ~41.67 ns (24 MHz)**. A single `submit()` completes in *less* than
one tick, so every per-op sample quantizes to 0 or one tick: the distribution
piles into the 0/41/42 ns buckets and **p50 and p95 collapse to the same value**.
The result is not a measurement of the engine — it is a measurement of the clock.
The clock-resolution guard in `run_percentile_benchmark()` detects this (measured
granularity ≈ 41 ns) and refuses to run, so coarse-timer numbers can never be
committed by accident. Benchmark on Linux x86 (invariant-TSC, ns-resolution) instead.

## Measurement boundary
`order submitted → trades vector returned` — no I/O, no logging, no printing inside
the loop. Per-event latency is measured with the x86 timestamp counter (`rdtsc`,
lfence-serialized, overhead-subtracted) where available, falling back to
`std::chrono::steady_clock` on other architectures; throughput microbenchmarks use
Google Benchmark. See *Per-operation timing* above.

- **CPU:** 6 cores / 12 threads @ ~2.1 GHz. Caches: L1d 32 KiB ×6, L2 512 KiB ×6, L3 4 MiB ×2.
- **OS:** Windows 11
- **Compiler:** GCC 15.2 (MSYS2 UCRT64), `-O3 -march=native -DNDEBUG`, C++20

> Note on `-DNDEBUG`: the initial Release flags dropped it, so the first build ran with
> assertions live (Google Benchmark warned *"Library was built as DEBUG"*). It was added
> back before any numbers below were recorded, so these are honest optimized-build figures.

## Workloads

> **Correction.** The percentile harness was previously described as *crossing* a
> pre-loaded ask wall. It never did: a full-size wall (`qty 1,000,000`) is risk-rejected
> (`REJECT_SIZE`), so the book started empty and every order **rested**. The harness has
> always measured the resting-insert path — which is exactly where the object pool
> matters — so the historical pool numbers below stand as resting-path figures.

- **Percentile bench (1M events):** 1,000,000 limit orders **rest** across a band of 100
  price levels — no crossing (`max trades per submit: 0`). Each insert allocates a
  `std::list` node (object pool unless `LOB_NO_POOL`) **and** an `index_` `unordered_map`
  node (always the system allocator; ~20 rehashes over the run). This is the
  allocation-bound submit path, measured at percentiles.
- **`BM_SubmitLimit`:** resting limit orders across 10 price levels — the *same kind* of
  path (resting insert), a different book shape. So its ~95.9 ns mean and the percentile
  p50 (~40 ns) are the **same operation measured two ways**, not a contradiction: 10 levels
  vs 100, and Google-Benchmark mean vs per-op rdtsc.
- **`BM_Cancel`:** cancel + re-insert against a 10k-order book (allocator + index churn).

### Baseline A/B — object pool on vs off
Build twice, run the same harness, so the pool's effect is measured on identical hardware:
```
cmake -B build-pool   -DCMAKE_BUILD_TYPE=Release                  && cmake --build build-pool   -j
cmake -B build-nopool -DCMAKE_BUILD_TYPE=Release -DLOB_NO_POOL=ON && cmake --build build-nopool -j
taskset -c 1 ./build-pool/bench      # Allocator: object pool (arena)
taskset -c 1 ./build-nopool/bench    # Allocator: system (pool disabled)
```
`LOB_NO_POOL` reverts `OrderList` to `std::list<Order>` (system allocator). Because every
order rests, pool-off pays a `malloc` for the list node on *every* insert; pool-on draws
it from the arena (a chunk `malloc` only every 4096 nodes). The benchmark workflow runs
both variants and uploads both outputs.

### Why the tail is bimodal (p95 ≪ p99)
`run_percentile_benchmark()` classifies every slow sample (> 8× p50). The jump is **not**
new price-level creation (only ~100 levels over 1M inserts) and **not** the trades vector
(no crossing → no trades). It is **heap allocation on the insert**:
- the `std::list` node — removed by the object pool;
- the `index_` `unordered_map` node and its ~20 rehashes — **not** pool-served.

So the pool tightens the tail (fewer `malloc`s per insert), but a residual remains from the
O(1)-cancel hash index — a candidate for a future intrusive index. The per-run breakdown
(new-level / index-rehash / neither) prints under *Tail analysis*.

### Limitation — rdtsc overhead vs a ~40 ns operation
The measured `rdtsc` overhead is **~10 ns** (printed each run, subtracted per sample). At a
p50 near 40 ns that is a meaningful fraction of the signal: subtraction uses the *median*
empty-region cost, so per-read jitter still lands in the measurement. Treat sub-50 ns
figures as indicative, not exact. The tail (p99/p99.9) — where the pool's effect lives — is
far larger than the overhead and unaffected.

---

## Baseline — `std::map` levels + `std::list` FIFO queue (system allocator)

Percentile (1,000,000 events, failures: 0):
```
  p50:    100 ns
  p95:    200–300 ns
  p99:    600–1100 ns
  p99.9:  ~22,000 ns      <- sporadic malloc stalls in the tail
```
Google Benchmark (median of 3 runs):
```
  BM_SubmitLimit   ~200 ns   (198–285 ns)
  BM_Cancel        ~160 ns   (154–171 ns)
```

## After Opt 1 — object pool (arena-backed queue nodes, no hot-path malloc)

`std::list<Order>` now draws its nodes from a process-wide, lock-free free-list arena
(`NodeArena` + `PoolAllocator`, in `include/lob/pool.hpp`). Justified by the
single-threaded-matcher design decision: one owner ⇒ one arena ⇒ no locks. Once warm,
the resting/cancel paths never call into the system allocator.

Percentile (1,000,000 events, failures: 0):
```
  p50:    100 ns        (unchanged — cross path does not allocate; clock-resolution bound)
  p95:    100–400 ns
  p99:    200–900 ns
  p99.9:  ~6,000 ns     <- ~3–4x tighter tail (no malloc stalls)
```
Google Benchmark (median of 3 runs):
```
  BM_SubmitLimit   ~122 ns   (115–137 ns)   ~38% faster than baseline
  BM_Cancel        ~100 ns   ( 95–106 ns)   ~37% faster than baseline
```

### Why p50 didn't move but the tail and insert/cancel did
The headline percentile workload fully fills each order, so it never reaches the
"rest in book" branch and never allocates — its p50 is already at the `steady_clock`
measurement floor (~100 ns). The object pool's win is on the paths that *do* allocate:
the resting-order insert (`BM_SubmitLimit`, −38%), cancel (`BM_Cancel`, −37%), and most
importantly the **tail** (p99.9 from ~22 µs to ~6 µs), where the baseline was paying for
occasional `malloc`/`free` slow paths.

## Notes
- Synthetic workloads; numbers vary run-to-run (shared laptop, frequency scaling) — ranges
  shown, medians quoted.
- Not measured: NIC/kernel/scheduling latency, OS jitter.
- Sanitizer-clean validation (ASan/UBSan) runs in CI on Linux (see M10); the MSYS2/MinGW
  toolchain used here ships no `libasan`/`libubsan`.

## Considered next (Opt 2 — array-indexed price levels)
For a dense tick band around the touch, replace the `std::map<Price, Level>` with a flat
array indexed by `price - base`, tracking best bid/ask as integers. This turns the
best-level lookup from a red-black-tree walk into O(1), cache-line-friendly indexing and
is the path to sub-200 ns p50 on the *matching* hot path. Deferred to keep the M6 change
surgical and the test suite stable; the pool win above is the documented optimization.
