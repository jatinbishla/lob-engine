#!/usr/bin/env python3
"""Generate the LOB engine's README charts from the compiled `lobpy` module.

Produces three PNGs in images/:
  1. depth_chart.png     — cumulative bid/ask depth staircase (L2 book shape)
  2. latency_hist.png    — per-op latency distribution with p50/p99/p99.9 marks
  3. ofi_timeseries.png  — order-flow imbalance over a synthetic ITCH replay

Build lobpy first (see README), then:  PYTHONPATH=<dir-with-lobpy> python3 python/make_charts.py
The notebook notebooks/visualize.ipynb mirrors these cells interactively.
"""
import os
import struct
import numpy as np
import matplotlib.pyplot as plt

# lobpy is imported lazily inside the charts that need it (depth_chart,
# ofi_timeseries) so latency_hist() — which reads real bench data and needs no
# bindings — can run even when the module was built with -DLOB_BUILD_PYTHON=OFF.

# --- Validated palette (see the dataviz skill's reference instance) ---
BID   = "#2a78d6"   # blue  — bid pressure
ASK   = "#e34948"   # red   — ask pressure
INK   = "#0b0b0b"   # text-primary
MUTE  = "#52514e"   # text-secondary
GRID  = "#e6e6e3"
SURF  = "#fcfcfb"

plt.rcParams.update({
    "figure.facecolor": SURF, "axes.facecolor": SURF,
    "axes.edgecolor": MUTE, "axes.labelcolor": INK, "text.color": INK,
    "xtick.color": MUTE, "ytick.color": MUTE,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
    "axes.spines.top": False, "axes.spines.right": False,
    "font.size": 11, "figure.dpi": 130,
})

IMG = os.path.join(os.path.dirname(__file__), os.pardir, "images")
os.makedirs(IMG, exist_ok=True)


# ----------------------------------------------------------------------------
# 1. Depth chart — cumulative bid/ask staircase
# ----------------------------------------------------------------------------
def depth_chart():
    import lobpy as L
    book = L.OrderBook()
    bids = {9999: 120, 9998: 250, 9997: 400, 9996: 150, 9995: 300}
    asks = {10001: 100, 10002: 220, 10003: 180, 10004: 350, 10005: 200}
    oid = 0
    for px, q in bids.items():
        oid += 1; book.rest(L.Order(oid, L.Side.Buy, L.OrderType.Limit, px, q))
    for px, q in asks.items():
        oid += 1; book.rest(L.Order(oid, L.Side.Sell, L.OrderType.Limit, px, q))

    # Cumulative depth outward from the touch.
    bp = sorted(bids, reverse=True)
    bcum = np.cumsum([book.depth_at(L.Side.Buy, p) for p in bp])
    ap = sorted(asks)
    acum = np.cumsum([book.depth_at(L.Side.Sell, p) for p in ap])

    fig, ax = plt.subplots(figsize=(7.2, 4.0))
    ax.step(bp, bcum, where="mid", color=BID, lw=2, label="Bids")
    ax.fill_between(bp, bcum, step="mid", color=BID, alpha=0.15)
    ax.step(ap, acum, where="mid", color=ASK, lw=2, label="Asks")
    ax.fill_between(ap, acum, step="mid", color=ASK, alpha=0.15)

    best_bid, best_ask = book.best_bid(), book.best_ask()
    mid = (best_bid + best_ask) / 2
    ax.axvline(mid, color=MUTE, ls="--", lw=1)
    ax.text(mid, ax.get_ylim()[1] * 0.02, f"mid {mid:.1f} ", color=MUTE,
            fontsize=9, va="bottom", ha="right")

    ax.set_title("Order-Book Depth — cumulative size by price", color=INK, fontsize=13, loc="left")
    ax.set_xlabel("Price (ticks)"); ax.set_ylabel("Cumulative quantity")
    ax.legend(frameon=False, loc="center")  # sits in the empty spread gap
    fig.tight_layout()
    out = os.path.join(IMG, "depth_chart.png")
    fig.savefig(out, bbox_inches="tight"); plt.close(fig)
    print("wrote", out)


# ----------------------------------------------------------------------------
# 2. Latency histogram — distribution with percentile markers
# ----------------------------------------------------------------------------
def latency_hist():
    # Plotted from real per-op samples emitted by ./build/bench — no modeling.
    csv = os.path.join(os.path.dirname(__file__), os.pardir, "data", "latency_samples.csv")
    if not os.path.exists(csv):
        raise SystemExit(
            "data/latency_samples.csv not found — run ./build/bench first "
            "(from the repo root) to generate the per-op latency samples.")
    lat = np.loadtxt(csv, skiprows=1)  # skip the 'latency_ns' header

    # Percentiles computed directly from the loaded samples.
    p50, p99, p999 = np.percentile(lat, [50, 99, 99.9])
    fig, ax = plt.subplots(figsize=(7.2, 4.0))
    bins = np.logspace(np.log10(max(lat.min(), 1)), np.log10(lat.max()), 60)
    ax.hist(lat, bins=bins, color=BID, alpha=0.85, edgecolor=SURF, linewidth=0.4)
    ax.set_xscale("log")

    for x, name, col in [(p50, "p50", INK), (p99, "p99", MUTE), (p999, "p99.9", ASK)]:
        ax.axvline(x, color=col, ls="--", lw=1.4)
        ax.text(x, ax.get_ylim()[1] * 0.92, f" {name} {x:,.0f}ns",
                rotation=90, va="top", ha="right", color=col, fontsize=9)

    ax.set_title("Submit-path latency distribution", color=INK, fontsize=13, loc="left")
    ax.set_xlabel("Latency per order (ns, log scale) — 1,000,000 events")
    ax.set_ylabel("Count")
    fig.tight_layout()
    out = os.path.join(IMG, "latency_hist.png")
    fig.savefig(out, bbox_inches="tight"); plt.close(fig)
    print("wrote", out)


# ----------------------------------------------------------------------------
# 3. OFI time series — signal over a synthetic ITCH replay
# ----------------------------------------------------------------------------
def _add_msg(ref, side, sh, px):
    body = (b"A" + struct.pack(">H", 1) + struct.pack(">H", 0) + b"\x00" * 6
            + struct.pack(">Q", ref) + side + struct.pack(">I", sh)
            + b" " * 8 + struct.pack(">I", px))
    return struct.pack(">H", len(body)) + body

def _del_msg(ref):
    body = (b"D" + struct.pack(">H", 1) + struct.pack(">H", 0) + b"\x00" * 6
            + struct.pack(">Q", ref))
    return struct.pack(">H", len(body)) + body

def ofi_timeseries():
    import lobpy as L
    book = L.OrderBook()
    h = L.ItchHandler(book)
    rng = np.random.default_rng(3)
    ref = 0
    resting = {L.Side.Buy: [], L.Side.Sell: []}  # (ref, price)
    series = []

    # Seed a two-sided book.
    for px, q in [(9999, 150), (9998, 200)]:
        ref += 1; h.process_stream(_add_msg(ref, b"B", q, px)); resting[L.Side.Buy].append((ref, px))
    for px, q in [(10001, 150), (10002, 200)]:
        ref += 1; h.process_stream(_add_msg(ref, b"S", q, px)); resting[L.Side.Sell].append((ref, px))

    # Three regimes: balanced, bid-pressure burst, ask-pressure burst.
    plan = ([0.5] * 60) + ([0.82] * 80) + ([0.2] * 80) + ([0.5] * 60)
    for buy_prob in plan:
        buy = rng.random() < buy_prob
        active = L.Side.Buy if buy else L.Side.Sell
        opp    = L.Side.Sell if buy else L.Side.Buy
        px     = 9999 if buy else 10001
        code   = b"B" if buy else b"S"
        ref += 1
        h.process_stream(_add_msg(ref, code, int(rng.integers(40, 120)), px))
        resting[active].append((ref, px))
        # Flow on one side pulls liquidity off the other: delete an aging order
        # on the opposite side so the imbalance can actually invert regime to
        # regime (still pure book reconstruction — just faster depth turnover).
        if len(resting[opp]) > 2 and rng.random() < 0.7:
            r, _ = resting[opp].pop(0)
            h.process_stream(_del_msg(r))
        series.append(h.order_flow_imbalance())

    ofi = np.array(series)
    x = np.arange(len(ofi))
    fig, ax = plt.subplots(figsize=(7.2, 4.0))
    ax.axhline(0, color=MUTE, lw=1)
    ax.plot(x, ofi, color=INK, lw=1.3)
    ax.fill_between(x, ofi, 0, where=ofi >= 0, color=BID, alpha=0.30, interpolate=True)
    ax.fill_between(x, ofi, 0, where=ofi < 0, color=ASK, alpha=0.30, interpolate=True)
    ax.set_ylim(-1.05, 1.05)
    ax.text(0.01, 0.96, "bid pressure", transform=ax.transAxes, color=BID, fontsize=9, va="top")
    ax.text(0.01, 0.06, "ask pressure", transform=ax.transAxes, color=ASK, fontsize=9, va="bottom")
    ax.set_title("Order-Flow Imbalance over an ITCH replay", color=INK, fontsize=13, loc="left")
    ax.set_xlabel("Event #"); ax.set_ylabel("OFI  (Qbid−Qask)/(Qbid+Qask)")
    fig.tight_layout()
    out = os.path.join(IMG, "ofi_timeseries.png")
    fig.savefig(out, bbox_inches="tight"); plt.close(fig)
    print("wrote", out)


if __name__ == "__main__":
    latency_hist()   # real bench data; needs no bindings
    try:
        import lobpy  # noqa: F401
    except ImportError:
        print("lobpy not built — skipping depth_chart and ofi_timeseries "
              "(build with -DLOB_BUILD_PYTHON=ON to regenerate them)")
    else:
        depth_chart()
        ofi_timeseries()
    print("done")
