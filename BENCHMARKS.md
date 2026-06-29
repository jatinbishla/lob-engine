# BENCHMARKS.md

## Measurement boundary
`order submitted → trades vector returned` — no I/O, no logging, no printing inside
the loop. Per-event latency is measured with `std::chrono::steady_clock`; throughput
microbenchmarks use Google Benchmark.

- **CPU:** 6 cores / 12 threads @ ~2.1 GHz. Caches: L1d 32 KiB ×6, L2 512 KiB ×6, L3 4 MiB ×2.
- **OS:** Windows 11
- **Compiler:** GCC 15.2 (MSYS2 UCRT64), `-O3 -march=native -DNDEBUG`, C++20

> Note on `-DNDEBUG`: the initial Release flags dropped it, so the first build ran with
> assertions live (Google Benchmark warned *"Library was built as DEBUG"*). It was added
> back before any numbers below were recorded, so these are honest optimized-build figures.

## Workloads
- **Percentile bench (1M events):** 100 deep resting asks pre-loaded; each event is a
  buy that crosses and fully fills 1 lot. This path *matches but never rests*, so it does
  **no heap allocation** — it measures the matching loop + `std::map` best-level lookup.
- **`BM_SubmitLimit`:** repeated resting limit orders across 10 price levels. This path
  **allocates a queue node per order**, so it is the one that exercises the allocator.
- **`BM_Cancel`:** cancel + re-insert against a 10k-order book (allocator + index churn).

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
