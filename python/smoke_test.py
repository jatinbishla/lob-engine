#!/usr/bin/env python3
"""Minimal import + behavior check for the lobpy bindings (used by CI)."""
import lobpy as L


def main():
    b = L.OrderBook()
    b.submit(L.Order(1, L.Side.Sell, L.OrderType.Limit, 10_100, 50))
    r = b.submit(L.Order(2, L.Side.Buy, L.OrderType.Limit, 10_100, 30))
    assert len(r.trades) == 1, r.trades
    assert r.trades[0].quantity == 30 and r.trades[0].price == 10_100
    assert r.risk == L.RiskResult.OK
    assert b.depth_at(L.Side.Sell, 10_100) == 20

    rr = b.submit(L.Order(3, L.Side.Buy, L.OrderType.Limit, 10_100, 100_000))
    assert rr.risk == L.RiskResult.REJECT_SIZE and not rr.trades

    print("lobpy import + matching + risk OK")


if __name__ == "__main__":
    main()
