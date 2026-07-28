#include <benchmark/benchmark.h>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include "lob/order_book.hpp"

using namespace lob;

// Measure steady_clock's real tick granularity: the smallest non-zero gap
// between two consecutive reads. Sampled ~1000 times, minimum taken. If this
// exceeds the operation we intend to time, the benchmark is meaningless — the
// clock quantizes every sample to 0 or one tick, so p50 and p95 collapse to the
// same value. (Apple Silicon's mach timebase ticks at ~41.67 ns; see BENCHMARKS.md.)
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
    // Refuse to produce meaningless numbers on a coarse timer.
    const int64_t granularity = measure_clock_granularity_ns();
    if (granularity > 10) {
        std::cerr << "\n!!! CLOCK RESOLUTION TOO COARSE — ABORTING BENCHMARK !!!\n";
        std::cerr << "steady_clock tick granularity: " << granularity << " ns\n";
        std::cerr << "A single submit() is faster than one clock tick, so every sample\n"
                     "quantizes to 0 or one tick and p50/p95 collapse to the same value —\n"
                     "the measurement is meaningless on this hardware.\n";
        std::cerr << "Run on a machine with a finer timer (Linux x86; see BENCHMARKS.md).\n";
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
        auto t0 = std::chrono::steady_clock::now();
        auto r = book.submit({++id, Side::Buy, OrderType::Limit, 10'100, 1});
        auto t1 = std::chrono::steady_clock::now();
        int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
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
