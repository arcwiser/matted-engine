#!/usr/bin/env python3
"""Matted Engine profiler report (TASK-19 companion tool).

Parses mtt::prof::report_top output - lines like:

      prof render_queue.cull      1.234 ms  (512 calls)
      prof physics.solve          0.871 ms  (120 calls)

emitted via log::info("  prof %-28s %8.3f ms  (%llu calls)", ...) from
engine/core/profiler.cpp - and aggregates every zone across all snapshots
found in the input, printing a sorted top-N cost table (total ms, average
per snapshot, calls, % share of total measured time).

Stdlib only. Reads files given on the command line, or stdin if none / "-".

Usage:
  build\\demo.exe | python tools/perf/prof_report.py --top 10
  python tools/perf/prof_report.py bench_log.txt debug_log.txt --sort calls
  python tools/perf/prof_report.py bench_log.txt --json
"""

from __future__ import annotations

import argparse
import json
import re
import sys

# log format: "  prof %-28s %8.3f ms  (%llu calls)"
# name is non-greedy so multi-word zone names stay intact.
RE_PROF = re.compile(
    r"^\s*prof\s+(.+?)\s+(\d+(?:\.\d+)?)\s*ms\s+\(\s*(\d+)\s+calls?\s*\)\s*$",
    re.IGNORECASE,
)


def parse(text: str) -> list:
    """Return [(zone_name, ms, calls)] for every prof line found."""
    out = []
    for line in text.splitlines():
        m = RE_PROF.match(line)
        if m:
            out.append((m.group(1).strip(), float(m.group(2)), int(m.group(3))))
    return out


def aggregate(rows: list) -> dict:
    """rows: [(name, ms, calls)] -> {name: {total_ms, calls, snapshots}}."""
    zones: dict[str, dict] = {}
    for name, ms, calls in rows:
        z = zones.setdefault(name, {"total_ms": 0.0, "calls": 0, "snapshots": 0})
        z["total_ms"] += ms
        z["calls"] += calls
        z["snapshots"] += 1
    return zones


def report(zones: dict, top: int, sort_key: str) -> list:
    rows = []
    grand = sum(z["total_ms"] for z in zones.values())
    for name, z in zones.items():
        rows.append({
            "name": name,
            "total_ms": z["total_ms"],
            "calls": z["calls"],
            "snapshots": z["snapshots"],
            "avg_ms": z["total_ms"] / z["snapshots"] if z["snapshots"] else 0.0,
            "avg_calls": z["calls"] / z["snapshots"] if z["snapshots"] else 0.0,
            "share_pct": 100.0 * z["total_ms"] / grand if grand else 0.0,
        })
    keymap = {
        "ms": lambda r: (-r["total_ms"], r["name"]),
        "calls": lambda r: (-r["calls"], r["name"]),
        "name": lambda r: r["name"],
    }
    rows.sort(key=keymap[sort_key])
    return rows[:top] if top > 0 else rows


def print_table(rows: list) -> None:
    if not rows:
        print("no 'prof ... ms (N calls)' lines found in input")
        return
    print(f"{'#':>3}  {'zone':<32} {'total ms':>10} {'avg ms':>9} "
          f"{'calls':>10} {'avg calls':>10} {'share':>7}")
    for i, r in enumerate(rows, 1):
        print(f"{i:>3}  {r['name']:<32.32} {r['total_ms']:>10.3f} "
              f"{r['avg_ms']:>9.3f} {r['calls']:>10,} "
              f"{r['avg_calls']:>10,.1f} {r['share_pct']:>6.1f}%")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Aggregate mtt::prof::report_top dumps into a top-N table")
    ap.add_argument("files", nargs="*",
                    help="log files to parse (default: stdin; '-' = stdin)")
    ap.add_argument("--top", type=int, default=20,
                    help="rows to show, 0 = all (default 20)")
    ap.add_argument("--sort", choices=("ms", "calls", "name"), default="ms")
    ap.add_argument("--json", action="store_true", help="print JSON instead")
    args = ap.parse_args(argv)

    rows = []
    paths = args.files or ["-"]
    try:
        for path in paths:
            if path == "-":
                rows.extend(parse(sys.stdin.read()))
            else:
                with open(path, "r", encoding="utf-8", errors="replace") as fh:
                    rows.extend(parse(fh.read()))
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    zones = aggregate(rows)
    result = report(zones, args.top, args.sort)
    if args.json:
        print(json.dumps({"total_ms": sum(z["total_ms"] for z in zones.values()),
                          "snapshots": max((z["snapshots"] for z in zones.values()),
                                           default=0),
                          "zones": result}, indent=1))
    else:
        print_table(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
