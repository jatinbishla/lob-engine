#!/usr/bin/env python3
"""Write benchmark results back into README.md and BENCHMARKS.md.

Parses the two bench outputs (object pool ON / OFF), each of which reports two
workloads — "resting insert" and "crossing/matching" — plus the Google Benchmark
insert/cancel figures and the observed CPU frequency. Builds comparison tables,
stamps provenance (CPU model, per-variant core frequency, TSC frequency, rdtsc
overhead, load average, run date), adds the frequency-scaling caveat, and replaces
the content between the BENCH:START / BENCH:END markers in both files.

Usage:
    python3 python/update_readme.py [pool_output] [nopool_output]
    (defaults: bench_output_pool.txt  bench_output_nopool.txt)

Fails loudly (non-zero) if the markers are missing — it never appends.
"""
import datetime as _dt
import os
import re
import sys

MARK_START = "<!-- BENCH:START -->"
MARK_END = "<!-- BENCH:END -->"
TARGETS = ["README.md", "BENCHMARKS.md"]
WORKLOADS = ["resting insert", "crossing/matching"]
PCTS = ["p50", "p95", "p99", "p99.9"]


def _search(text, pattern, group=1):
    m = re.search(pattern, text)
    return m.group(group) if m else None


def parse_workload(text, label):
    """Percentiles for one '=== Latency percentiles: <label> ===' section."""
    m = re.search(r"=== Latency percentiles: " + re.escape(label) + r" ===(.*?)(?:\n===|\Z)",
                  text, re.S)
    body = m.group(1) if m else ""
    return {pct: _search(body, rf"{re.escape(pct)}:\s*(\d+)\s*ns") for pct in PCTS}


def parse_bench(path):
    if not os.path.exists(path):
        sys.exit(f"ERROR: bench output not found: {path} (run ./build*/bench first)")
    text = open(path, encoding="utf-8").read()
    d = {
        "allocator": _search(text, r"Allocator:\s*(.+)") or "?",
        "tsc_ghz": _search(text, r"calibrated\s*([\d.]+)\s*GHz"),
        "overhead_ns": _search(text, r"rdtsc overhead:\s*([\d.]+)\s*ns"),
        # Prefer the bench's explicitly-logged core frequency; fall back to
        # Google Benchmark's "Run on (N X FREQ MHz CPU s)" context line.
        "cpu_mhz": (_search(text, r"CPU frequency \(observed\):\s*([\d.]+)\s*MHz")
                    or _search(text, r"Run on \(\d+ X\s*([\d.]+)\s*MHz")),
        "insert": _search(text, r"BM_SubmitLimit\s+([\d.]+)\s*ns"),
        "cancel": _search(text, r"BM_Cancel\s+([\d.]+)\s*ns"),
        # 1-minute load average recorded (by benchmark.yml) immediately before the run.
        "load_before": _search(text, r"load before .*? run:\s*([\d.]+)"),
    }
    for w in WORKLOADS:
        d[w] = parse_workload(text, w)
    return d


def cpu_model():
    try:
        for line in open("/proc/cpuinfo", encoding="utf-8"):
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    import platform
    return platform.processor() or platform.machine() or "unknown"


def cell(v, unit=" ns"):
    return f"{v}{unit}" if v is not None else "n/a"


def _close(a, b, atol=0.0, rtol=0.0):
    """True if a and b parse to floats that agree within the given tolerance."""
    try:
        fa, fb = float(a), float(b)
    except (TypeError, ValueError):
        return False
    return abs(fa - fb) <= atol + rtol * max(fa, fb)


def workload_table(title, note, pool_w, nopool_w):
    lines = [f"**{title}** — {note}", "",
             "| Metric | Object pool | System allocator |", "|:--|--:|--:|"]
    for pct in PCTS:
        lines.append(f"| {pct} | {cell(pool_w[pct])} | {cell(nopool_w[pct])} |")
    return "\n".join(lines)


def build_block(pool, nopool):
    # On the crossing path allocation is off the critical path, so the pool is not
    # expected to help — note that under the crossing table with the actual p99.9s.
    cx_note = (
        f"_On the matching path allocation is off the critical path, so the object pool "
        f"shows no tail advantage here (p99.9 {cell(pool['crossing/matching']['p99.9'])} "
        f"vs {cell(nopool['crossing/matching']['p99.9'])})._"
    )
    parts = [
        workload_table("Resting insert",
                       "allocation-bound; the object pool's path",
                       pool["resting insert"], nopool["resting insert"]),
        "",
        workload_table("Crossing / matching",
                       "aggressive orders fill against resting liquidity",
                       pool["crossing/matching"], nopool["crossing/matching"]),
        "",
        cx_note,
        "",
        "**Throughput** (Google Benchmark)", "",
        "| Metric | Object pool | System allocator |",
        "|:--|--:|--:|",
        f"| insert · BM_SubmitLimit | {cell(pool['insert'])} | {cell(nopool['insert'])} |",
        f"| cancel · BM_Cancel | {cell(pool['cancel'])} | {cell(nopool['cancel'])} |",
    ]

    date = _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    pool_mhz, nopool_mhz = cell(pool["cpu_mhz"], " MHz"), cell(nopool["cpu_mhz"], " MHz")
    pool_la, nopool_la = pool["load_before"], nopool["load_before"]
    meta = (
        f"\n_Runner: **{cpu_model()}** · core clock pool {pool_mhz} / no-pool {nopool_mhz} · "
        f"TSC {cell(pool['tsc_ghz'], ' GHz')} · rdtsc overhead {cell(pool['overhead_ns'])} · "
        f"load-before pool {cell(pool_la, '')} / no-pool {cell(nopool_la, '')} · "
        f"{date} · 1,000,000 events/workload, core-pinned._"
    )

    # Fix 1: only warn about frequency scaling when the two variants actually ran
    # at different clocks; otherwise confirm the comparison was clock-controlled.
    if _close(pool["cpu_mhz"], nopool["cpu_mhz"], rtol=0.01):
        clock_note = (
            f"\n> **Clock-controlled.** Both variants ran at the same core clock "
            f"({pool_mhz}), so the p50/p95 comparison across allocators is not confounded "
            f"by frequency scaling."
        )
    else:
        clock_note = (
            "\n> **Frequency-scaling caveat.** The two variants ran at different core clocks "
            f"(pool {pool_mhz} vs no-pool {nopool_mhz} on this shared runner), so the **p50/p95 "
            "comparison across allocators is confounded by clock, not just allocator**. The "
            "**p99.9 comparison is the robust one**: tail behaviour is allocator-dominated "
            "(malloc / arena grow), not clock-dominated."
        )

    # Fix 2: if the load average differed between runs, flag it as a residual caveat
    # on the tail comparison even when the clock was controlled.
    load_note = ""
    if pool_la and nopool_la and not _close(pool_la, nopool_la, atol=0.05):
        load_note = (
            f"\n> **Residual caveat.** The two runs saw different load averages "
            f"({pool_la} vs {nopool_la}), which can still perturb the tail (p99.9) comparison "
            f"even at a controlled clock."
        )

    return "\n".join(parts) + "\n" + meta + "\n" + clock_note + load_note


def replace_between_markers(path, block):
    if not os.path.exists(path):
        sys.exit(f"ERROR: target file not found: {path}")
    text = open(path, encoding="utf-8").read()
    if MARK_START not in text or MARK_END not in text:
        sys.exit(f"ERROR: markers {MARK_START} / {MARK_END} not found in {path} "
                 f"— refusing to append.")
    pre, rest = text.split(MARK_START, 1)
    _, post = rest.split(MARK_END, 1)
    new = f"{pre}{MARK_START}\n{block.strip()}\n{MARK_END}{post}"
    open(path, "w", encoding="utf-8").write(new)
    print(f"updated {path}")


def main():
    pool_path = sys.argv[1] if len(sys.argv) > 1 else "bench_output_pool.txt"
    nopool_path = sys.argv[2] if len(sys.argv) > 2 else "bench_output_nopool.txt"
    block = build_block(parse_bench(pool_path), parse_bench(nopool_path))
    for target in TARGETS:
        replace_between_markers(target, block)


if __name__ == "__main__":
    main()
