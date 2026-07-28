#include <benchmark/benchmark.h>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <fstream>
#include <string>
#include "lob/order_book.hpp"

// Per-op timing backend: the x86 timestamp counter (rdtsc) where available,
// std::chrono::steady_clock elsewhere. clock_gettime costs ~20 ns/call even
// through the vDSO — a large fraction of the operation we're timing — so on x86
// we read the TSC directly with lfence serialization and subtract the measured
// overhead. (See BENCHMARKS.md.)
#if defined(__x86_64__) || defined(__i386__)
  #define LOB_USE_RDTSC 1
  #include <x86intrin.h>
#else
  #define LOB_USE_RDTSC 0
#endif

using namespace lob;

#if LOB_USE_RDTSC

// Calibrate TSC ticks-per-nanosecond against steady_clock over ~200 ms. The TSC
// runs at a fixed rate (invariant TSC), unrelated to core frequency, so it must
// be calibrated at runtime rather than assumed from the nominal clock.
static double calibrate_tsc_ticks_per_ns() {
    using namespace std::chrono;
    _mm_lfence();
    const uint64_t c0 = __rdtsc();
    _mm_lfence();
    const auto t0 = steady_clock::now();
    std::this_thread::sleep_for(milliseconds(200));
    _mm_lfence();
    const uint64_t c1 = __rdtsc();
    _mm_lfence();
    const auto t1 = steady_clock::now();
    const double ns = static_cast<double>(duration_cast<nanoseconds>(t1 - t0).count());
    return static_cast<double>(c1 - c0) / ns;   // ticks per nanosecond
}

// Median cost (in ticks) of an empty timed region — two back-to-back rdtsc reads
// bracketed by lfence, identical in shape to the real measurement minus submit().
// Subtracted from every sample so we report the operation, not the instrument.
static uint64_t measure_rdtsc_overhead_ticks() {
    const int N = 10'000;
    std::vector<uint64_t> deltas;
    deltas.reserve(N);
    for (int i = 0; i < N; ++i) {
        _mm_lfence();
        const uint64_t a = __rdtsc();
        const uint64_t b = __rdtsc();
        _mm_lfence();
        deltas.push_back(b - a);
    }
    std::sort(deltas.begin(), deltas.end());
    return deltas[deltas.size() / 2];
}

#else  // fallback: steady_clock

// Smallest non-zero gap between two consecutive steady_clock reads (ns) — the
// timer's real tick granularity. Sampled ~1000 times, minimum taken.
static int64_t measure_clock_granularity_ns() {
    int64_t min_delta = INT64_MAX;
    for (int i = 0; i < 1000; ++i) {
        const auto a = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point b;
        do { b = std::chrono::steady_clock::now(); } while (b == a); // spin until it advances
        const int64_t d =
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
        if (d > 0 && d < min_delta) min_delta = d;
    }
    return min_delta;
}

#endif

// --- Google Benchmark: throughput ---
static void BM_SubmitLimit(benchmark::State& state) {
    OrderBook book;
    OrderId id = 0;
    for (auto _ : state)
        book.submit({++id, Side::Buy, OrderType::Limit, static_cast<Price>(100 + (id % 10)), 10});
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SubmitLimit)->Unit(benchmark::kNanosecond);

static void BM_Cancel(benchmark::State& state) {
    OrderBook book;
    const int N = 10'000;
    for (int i = 1; i <= N; ++i)
        book.submit({(OrderId)i, Side::Buy, OrderType::Limit, 100, 10});
    OrderId id = 1;
    for (auto _ : state) {
        book.cancel(id);
        // re-insert to keep book populated
        book.submit({id + N, Side::Buy, OrderType::Limit, 100, 10});
        ++id;
    }
}
BENCHMARK(BM_Cancel)->Unit(benchmark::kNanosecond);

// Observed core frequency in MHz (Linux only). Core frequency scales on shared
// runners and is NOT the invariant TSC rate, so it is logged separately: a large
// clock difference between the two variant runs confounds cross-variant p50/p95
// comparison (see BENCHMARKS.md).
static double read_cpu_mhz() {
#if defined(__linux__)
    if (std::ifstream f("/sys/devices/system/cpu/cpu1/cpufreq/scaling_cur_freq"); f) {
        double khz = 0;
        if (f >> khz) return khz / 1000.0;   // pinned core's current frequency
    }
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("cpu MHz", 0) == 0) {
            const auto pos = line.find(':');
            if (pos != std::string::npos) return std::atof(line.c_str() + pos + 1);
        }
#endif
    return 0.0;
}

// Shared timing backend, set up once and reused by every workload.
struct Timing {
#if LOB_USE_RDTSC
    double   ticks_per_ns   = 0;
    uint64_t overhead_ticks = 0;
#endif
    double resolution_ns = 0;
};

static Timing setup_timing() {
    Timing tm;
#if LOB_USE_RDTSC
    tm.ticks_per_ns   = calibrate_tsc_ticks_per_ns();
    tm.resolution_ns  = 1.0 / tm.ticks_per_ns;               // one TSC tick, in ns
    tm.overhead_ticks = measure_rdtsc_overhead_ticks();
    std::cout << "Timer: rdtsc  calibrated " << tm.ticks_per_ns << " GHz ("
              << tm.ticks_per_ns << " ticks/ns)\n";
    std::cout << "rdtsc overhead: " << (tm.overhead_ticks / tm.ticks_per_ns)
              << " ns (subtracted from every sample)\n";
#else
    tm.resolution_ns = static_cast<double>(measure_clock_granularity_ns());
    std::cout << "Timer: steady_clock  granularity " << tm.resolution_ns << " ns\n";
#endif
    if (tm.resolution_ns > 10.0) {          // refuse meaningless numbers on a coarse timer
        std::cerr << "\n!!! CLOCK RESOLUTION TOO COARSE — ABORTING BENCHMARK !!!\n";
        std::cerr << "effective timer resolution: " << tm.resolution_ns << " ns\n";
        std::cerr << "A single submit() is faster than one timer tick, so every sample\n"
                     "quantizes to 0 or one tick and p50/p95 collapse to the same value —\n"
                     "the measurement is meaningless on this hardware.\n";
        std::cerr << "Run on a machine with a finer timer (Linux x86 rdtsc; see BENCHMARKS.md).\n";
        std::exit(1);
    }
    return tm;
}

// Per-op latencies plus a per-sample cause tag, filled by measure().
struct Samples {
    std::vector<int64_t> latency;   // submit order
    std::vector<uint8_t> cause;     // 0=neither, 1=new level, 2=index rehash
    size_t levels_created = 0, index_rehashes = 0, max_trades = 0, rejected = 0;
};

// Run N timed submit()s. The next order is produced OUTSIDE the timed region by
// `next` (so its cost is never measured); the timed region brackets only submit()
// with lfence/rdtsc (or steady_clock on the fallback). The level/bucket reads that
// classify each sample also sit outside the timed region.
static Samples measure([[maybe_unused]] const Timing& tm, OrderBook& book,
                       const std::function<Order(int)>& next, int N) {
    Samples s;
    s.latency.reserve(N);
    s.cause.reserve(N);
    for (int i = 0; i < N; ++i) {
        const Order o = next(i);
        const std::size_t lv_before  = book.level_count();
        const std::size_t bkt_before = book.index_bucket_count();
#if LOB_USE_RDTSC
        _mm_lfence();
        const uint64_t t0 = __rdtsc();
        auto r = book.submit(o);
        const uint64_t t1 = __rdtsc();
        _mm_lfence();
        const uint64_t raw = t1 - t0;
        const uint64_t adj = (raw > tm.overhead_ticks) ? raw - tm.overhead_ticks : 0;
        const int64_t ns = static_cast<int64_t>(adj / tm.ticks_per_ns + 0.5);
#else
        const auto t0 = std::chrono::steady_clock::now();
        auto r = book.submit(o);
        const auto t1 = std::chrono::steady_clock::now();
        const int64_t ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
#endif
        const bool made_level = book.level_count() > lv_before;
        const bool rehashed   = book.index_bucket_count() > bkt_before;
        if (made_level) ++s.levels_created;
        if (rehashed)   ++s.index_rehashes;
        if (r.trades.size() > s.max_trades) s.max_trades = r.trades.size();
        if (r.risk != RiskResult::OK)       ++s.rejected;
        s.latency.push_back(ns);
        s.cause.push_back(made_level ? 1 : (rehashed ? 2 : 0));
    }
    return s;
}

// Sort, print the labelled percentile block + tail breakdown, and optionally dump
// the samples CSV that feeds the histogram.
static void report(const std::string& label, const char* allocator_label,
                   Samples& s, int N, bool write_csv) {
    std::vector<int64_t> sorted = s.latency;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) -> int64_t {
        return sorted[static_cast<size_t>(p / 100.0 * (sorted.size() - 1))];
    };
    const int64_t p50 = pct(50);

    std::cout << "\n=== Latency percentiles: " << label << " ===\n";
    std::cout << "Allocator: " << allocator_label
              << "   Events: " << N << "   Rejected: " << s.rejected << "\n";
    std::cout << "  p50:    " << p50       << " ns\n";
    std::cout << "  p95:    " << pct(95)   << " ns\n";
    std::cout << "  p99:    " << pct(99)   << " ns\n";
    std::cout << "  p99.9:  " << pct(99.9) << " ns\n";

    const int64_t thr = std::max<int64_t>(200, 8 * p50);
    size_t slow = 0, sl = 0, sr = 0, so = 0;
    for (size_t i = 0; i < s.latency.size(); ++i)
        if (s.latency[i] > thr) {
            ++slow;
            if      (s.cause[i] == 1) ++sl;
            else if (s.cause[i] == 2) ++sr;
            else                      ++so;
        }
    std::cout << "  tail (> " << thr << " ns): " << slow << " total | new-level " << sl
              << " [" << s.levels_created << "] | index-rehash " << sr
              << " [" << s.index_rehashes << "] | node-malloc/jitter " << so << "\n";
    std::cout << "  max trades per submit: " << s.max_trades
              << (s.max_trades == 0 ? "  (pure resting path)\n" : "  (crossing path)\n");

    if (write_csv) {
        const char* csv_path = "data/latency_samples.csv";
        std::ofstream csv(csv_path);
        if (csv) {
            csv << "latency_ns\n";
            for (int64_t ns : sorted) csv << ns << '\n';
            std::cout << "  wrote " << sorted.size() << " samples to " << csv_path << "\n";
        } else {
            std::cout << "  WARNING: could not open " << csv_path
                      << " (run from the repo root so data/ exists)\n";
        }
    }
}

// Pre-populate two-sided resting liquidity for the crossing workload. Uses the
// feed-replay rest() path (no risk, no matching), so a deep book can be built
// without tripping the size/notional risk limits; aggressive orders then cross it.
static void preload_crossing(OrderBook& book, OrderId& id) {
    for (int k = 0; k < 6000; ++k) {
        book.rest({++id, Side::Sell, OrderType::Limit,
                   static_cast<Price>(10'100 + (k % 100)), 100});   // asks the buys will hit
        book.rest({++id, Side::Buy,  OrderType::Limit,
                   static_cast<Price>(9'900  + (k % 100)), 100});   // bids the sells will hit
    }
}

// --- Manual percentile benchmark: two workloads over one shared timer ---
void run_percentile_benchmark() {
    const Timing tm = setup_timing();
    const double cpu_mhz = read_cpu_mhz();
    std::cout << "CPU frequency (observed): "
              << (cpu_mhz > 0 ? std::to_string(cpu_mhz) + " MHz" : std::string("n/a")) << "\n";

#ifdef LOB_NO_POOL
    const char* allocator_label = "system (pool disabled)";
#else
    const char* allocator_label = "object pool (arena)";
#endif
    std::cout << "Allocator: " << allocator_label << "\n";

    const int N = 1'000'000;

    // Workload 1 — resting insert: no liquidity to cross, so every order rests and
    // each submit allocates a std::list node (object pool unless LOB_NO_POOL) plus
    // an index_ unordered_map node. Spread across a 100-level band. Feeds the histogram.
    {
        OrderBook book;
        OrderId id = 0;
        Samples s = measure(tm, book,
            [&](int i) {
                return Order{++id, Side::Buy, OrderType::Limit,
                             static_cast<Price>(10'000 + (i % 100)), 10};
            }, N);
        report("resting insert", allocator_label, s, N, /*write_csv=*/true);
    }

    // Workload 2 — crossing/matching: pre-load a two-sided book, then submit
    // aggressive orders that alternate buy/sell so they actually fill (max trades
    // > 0) while net inventory stays within the risk limit. Exercises the match
    // loop + trades-vector allocation + node free, not the resting-insert path.
    {
        OrderBook book;
        OrderId id = 0;
        preload_crossing(book, id);
        Samples s = measure(tm, book,
            [&](int i) {
                return (i % 2 == 0)
                    ? Order{++id, Side::Buy,  OrderType::Limit, 10'199, 1}  // crosses asks
                    : Order{++id, Side::Sell, OrderType::Limit,  9'900, 1}; // crosses bids
            }, N);
        report("crossing/matching", allocator_label, s, N, /*write_csv=*/false);
    }

    std::cout << "Boundary: order submitted -> trades vector returned (no I/O, no logging)\n";
}

int main(int argc, char** argv) {
    // Run percentile bench first
    run_percentile_benchmark();

    // Then Google Benchmark
    ::benchmark::Initialize(&argc, argv);
    ::benchmark::RunSpecifiedBenchmarks();
    return 0;
}
