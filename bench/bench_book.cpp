#include <benchmark/benchmark.h>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <fstream>
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

// --- Manual percentile benchmark (1M events) ---
void run_percentile_benchmark() {
    // Set up the timing backend and determine the timer's effective resolution
    // in ns (one TSC tick for rdtsc; the tick granularity for steady_clock).
#if LOB_USE_RDTSC
    const double ticks_per_ns   = calibrate_tsc_ticks_per_ns();
    const double resolution_ns  = 1.0 / ticks_per_ns;      // one TSC tick, in ns
    const uint64_t overhead_ticks = measure_rdtsc_overhead_ticks();
    const double overhead_ns    = overhead_ticks / ticks_per_ns;
    std::cout << "Timer: rdtsc  calibrated " << ticks_per_ns << " GHz ("
              << ticks_per_ns << " ticks/ns)\n";
    std::cout << "rdtsc overhead: " << overhead_ns
              << " ns (subtracted from every sample)\n";
#else
    const double resolution_ns = static_cast<double>(measure_clock_granularity_ns());
    std::cout << "Timer: steady_clock  granularity " << resolution_ns << " ns\n";
#endif

    // Refuse to produce meaningless numbers on a coarse timer.
    if (resolution_ns > 10.0) {
        std::cerr << "\n!!! CLOCK RESOLUTION TOO COARSE — ABORTING BENCHMARK !!!\n";
        std::cerr << "effective timer resolution: " << resolution_ns << " ns\n";
        std::cerr << "A single submit() is faster than one timer tick, so every sample\n"
                     "quantizes to 0 or one tick and p50/p95 collapse to the same value —\n"
                     "the measurement is meaningless on this hardware.\n";
        std::cerr << "Run on a machine with a finer timer (Linux x86 rdtsc; see BENCHMARKS.md).\n";
        std::exit(1);
    }

    const int N = 1'000'000;
    OrderBook book;
    std::vector<int64_t> latencies;
    latencies.reserve(N);

    OrderId id = 0;
    int failures = 0;

    // Pre-populate with some asks to cross against
    for (int i = 0; i < 100; ++i)
        book.submit({++id, Side::Sell, OrderType::Limit, 10'100 + i, 1'000'000});

    for (int i = 0; i < N; ++i) {
#if LOB_USE_RDTSC
        _mm_lfence();
        const uint64_t t0 = __rdtsc();
        auto r = book.submit({++id, Side::Buy, OrderType::Limit, 10'100, 1});
        const uint64_t t1 = __rdtsc();
        _mm_lfence();
        const uint64_t raw = t1 - t0;
        const uint64_t adj = (raw > overhead_ticks) ? raw - overhead_ticks : 0;
        const int64_t ns = static_cast<int64_t>(adj / ticks_per_ns + 0.5);
#else
        const auto t0 = std::chrono::steady_clock::now();
        auto r = book.submit({++id, Side::Buy, OrderType::Limit, 10'100, 1});
        const auto t1 = std::chrono::steady_clock::now();
        const int64_t ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
#endif
        latencies.push_back(ns);
        if (r.trades.empty() && r.risk != RiskResult::OK) ++failures;
    }

    std::sort(latencies.begin(), latencies.end());

    auto percentile = [&](double p) -> int64_t {
        size_t idx = static_cast<size_t>(p / 100.0 * (latencies.size() - 1));
        return latencies[idx];
    };

    std::cout << "\n=== LOB Engine Latency Benchmark ===\n";
    std::cout << "Core: 0  Events: 1,000,000  Failures: " << failures << "\n";
    std::cout << "  p50:    " << percentile(50)  << " ns\n";
    std::cout << "  p95:    " << percentile(95)  << " ns\n";
    std::cout << "  p99:    " << percentile(99)  << " ns\n";
    std::cout << "  p99.9:  " << percentile(99.9) << " ns\n";
    std::cout << "Classification: price-time priority matching, object pool (arena), std::map levels\n";
    std::cout << "Boundary: order submitted -> trades vector returned (no I/O, no logging)\n";

    // Dump raw per-op samples so the histogram is plotted from real data
    // (see python/make_charts.py), not a modeled distribution.
    const char* csv_path = "data/latency_samples.csv";
    std::ofstream csv(csv_path);
    if (csv) {
        csv << "latency_ns\n";
        for (int64_t ns : latencies) csv << ns << '\n';
        std::cout << "Wrote " << latencies.size() << " samples to " << csv_path << "\n";
    } else {
        std::cout << "WARNING: could not open " << csv_path
                  << " (run from the repo root so data/ exists)\n";
    }
}

int main(int argc, char** argv) {
    // Run percentile bench first
    run_percentile_benchmark();

    // Then Google Benchmark
    ::benchmark::Initialize(&argc, argv);
    ::benchmark::RunSpecifiedBenchmarks();
    return 0;
}
