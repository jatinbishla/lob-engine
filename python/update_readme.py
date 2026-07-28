#!/usr/bin/env python3
"""Write benchmark results back into README.md and BENCHMARKS.md.

Parses the two bench outputs (object pool ON / OFF), builds a comparison table
across p50/p95/p99/p99.9 plus the Google Benchmark insert/cancel figures, stamps
the run provenance (CPU model, TSC frequency, rdtsc overhead, load average, run
date), and replaces the content between the BENCH:START / BENCH:END markers in
both files.

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


def parse_bench(path):
    """Extract percentiles, TSC/overhead, allocator, and GB rows from one run."""
    if not os.path.exists(path):
        sys.exit(f"ERROR: bench output not found: {path} (run ./build*/bench first)")
    text = open(path, encoding="utf-8").read()

    def find(pattern, group=1):
        m = re.search(pattern, text)
        return m.group(group) if m else None

    d = {
        "allocator": find(r"Allocator:\s*(.+)") or "?",
        "tsc_ghz": find(r"calibrated\s*([\d.]+)\s*GHz"),
        "overhead_ns": find(r"rdtsc overhead:\s*([\d.]+)\s*ns"),
    }
    for pct in ("p50", "p95", "p99", "p99.9"):
        d[pct] = find(rf"{re.escape(pct)}:\s*(\d+)\s*ns")
    # Google Benchmark rows: "BM_SubmitLimit   95.9 ns   95.8 ns   7304832"
    d["insert"] = find(r"BM_SubmitLimit\s+([\d.]+)\s*ns")
    d["cancel"] = find(r"BM_Cancel\s+([\d.]+)\s*ns")
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


def load_average():
    try:
        return "{:.2f}, {:.2f}, {:.2f}".format(*os.getloadavg())
    except (OSError, AttributeError):
        return "n/a"


def cell(v, unit=" ns"):
    return f"{v}{unit}" if v is not None else "n/a"


def build_block(pool, nopool):
    rows = [
        ("p50", pool["p50"], nopool["p50"]),
        ("p95", pool["p95"], nopool["p95"]),
        ("p99", pool["p99"], nopool["p99"]),
        ("p99.9", pool["p99.9"], nopool["p99.9"]),
        ("insert · BM_SubmitLimit", pool["insert"], nopool["insert"]),
        ("cancel · BM_Cancel", pool["cancel"], nopool["cancel"]),
    ]
    lines = [
        "| Metric | Object pool | System allocator |",
        "|:--|--:|--:|",
    ]
    for name, a, b in rows:
        lines.append(f"| {name} | {cell(a)} | {cell(b)} |")

    date = _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    meta = (
        f"\n_Runner: **{cpu_model()}** · "
        f"TSC {cell(pool['tsc_ghz'], ' GHz')} · "
        f"rdtsc overhead {cell(pool['overhead_ns'])} · "
        f"load {load_average()} · {date} · "
        f"1,000,000 events, core-pinned. Object pool = `{pool['allocator']}`, "
        f"system = `{nopool['allocator']}`._"
    )
    return "\n".join(lines) + "\n" + meta


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
    pool = parse_bench(pool_path)
    nopool = parse_bench(nopool_path)
    block = build_block(pool, nopool)
    for target in TARGETS:
        replace_between_markers(target, block)


if __name__ == "__main__":
    main()
