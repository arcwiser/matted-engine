#!/usr/bin/env python3
"""Matted Engine perf-regression harness (TASK-19).

Runs build/bench.exe (render | physics) N times, parses timing/count metrics
from its output, keeps a JSON history on disk, diffs against the previous
run, and gates the medians against thresholds from a JSON file - failing
LOUDLY (FAIL banner + non-zero exit) when a threshold is breached.

Stdlib only. Works standalone until apps/bench.exe exists: use --selftest
or point --bin at a fake binary that prints synthetic output.

Primary parse target (proposed to AgentA, see tools/perf/README.md):

    BENCH render   p99_ms=7.42 mean_ms=5.10 fps=196.2 instances=1000000 draws=1200
    BENCH physics  p99_ms=3.10 mean_ms=2.20 bodies=10000 contacts=45000 steps=120

Fallback: tolerant regexes for prose-ish output ("frame p99: 12.3 ms",
"fps 143.2", "bodies: 10,000"). Keys found via the primary line always win.

Exit codes: 0 pass | 2 threshold breach | 3 could not run binary | 4 no metrics parsed.

Usage:
  python tools/perf/bench.py --suite both --check
  python tools/perf/bench.py --suite render --runs 5 --label "tile sweep 32"
  python tools/perf/bench.py --suite physics --diff
  python tools/perf/bench.py --selftest
"""

from __future__ import annotations

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time

EXIT_OK = 0
EXIT_BREACH = 2
EXIT_RUNFAIL = 3
EXIT_PARSE = 4

SUITES = ("render", "physics")

# --- parsing -----------------------------------------------------------------

# Primary summary line: "BENCH render p99_ms=7.42 mean_ms=5.10 fps=196.2 ..."
RE_BENCH_LINE = re.compile(r"^\s*BENCH\s+(render|physics)\b\s*(.*)$", re.IGNORECASE)
RE_KV = re.compile(
    r"([a-z_][a-z0-9_]*)\s*=\s*([-+]?\d[\d,_.]*(?:[eE][-+]?\d+)?)", re.IGNORECASE
)

# Tolerant fallbacks: metric -> regex with one capture group.
# {0,24} gaps allow things like "p99 frame time:", "p99 (ms):", "bodies =".
_COUNT = r"(\d[\d,_ ]*)"  # 10,000 / 1_000_000 / 100 000
_FALLBACK = {
    "p99_ms": re.compile(r"\bp99[^0-9\n]{0,24}?(\d+(?:\.\d+)?)\s*ms", re.I),
    "mean_ms": re.compile(r"\bmean[^0-9\n]{0,24}?(\d+(?:\.\d+)?)\s*ms", re.I),
    "avg_ms": re.compile(r"\bavg(?:erage)?[^0-9\n]{0,24}?(\d+(?:\.\d+)?)\s*ms", re.I),
    "min_ms": re.compile(r"\bmin(?:imum)?[^0-9\n]{0,24}?(\d+(?:\.\d+)?)\s*ms", re.I),
    "max_ms": re.compile(r"\bmax(?:imum)?[^0-9\n]{0,24}?(\d+(?:\.\d+)?)\s*ms", re.I),
    "fps": re.compile(r"\b(?:fps|frames?[/ ]s)\D{0,8}?(\d+(?:\.\d+)?)", re.I),
    "instances": re.compile(r"\binstances?\D{0,8}?" + _COUNT, re.I),
    "draws": re.compile(r"\bdraws?\D{0,8}?" + _COUNT, re.I),
    "culled": re.compile(r"\bculled\D{0,8}?" + _COUNT, re.I),
    "bodies": re.compile(r"\bbodies\D{0,8}?" + _COUNT, re.I),
    "contacts": re.compile(r"\bcontacts?\D{0,8}?" + _COUNT, re.I),
    "steps": re.compile(r"\bsteps?\D{0,8}?" + _COUNT, re.I),
    "frames": re.compile(r"\bframes?\D{0,8}?" + _COUNT, re.I),
}

# "_ms" metrics are per-frame times; counts are integers.
_COUNT_KEYS = frozenset(
    ["instances", "draws", "culled", "bodies", "contacts", "steps", "frames"]
)


def _to_count(text: str) -> float:
    return float(re.sub(r"[,_ ]", "", text))


def parse_metrics(text: str, suite_hint: str) -> dict:
    """Extract {metric: value} from one bench run's output.

    suite_hint attributes fallback matches when no primary BENCH line exists.
    """
    metrics: dict[str, float] = {}
    seen_primary = False

    for line in text.splitlines():
        m = RE_BENCH_LINE.match(line)
        if m:
            seen_primary = True
            suite = m.group(1).lower()
            if suite != suite_hint:
                continue  # other suite's summary line
            for key, val in RE_KV.findall(m.group(2)):
                try:  # tolerate 1,000,000 / 1_000_000 in counts
                    metrics[key.lower()] = float(re.sub(r"[,_]", "", val))
                except ValueError:
                    continue

    if not seen_primary:
        for key, rx in _FALLBACK.items():
            m = rx.search(text)
            if m:
                raw = m.group(1)
                metrics[key] = _to_count(raw) if key in _COUNT_KEYS else float(raw)
    return metrics


# --- running -----------------------------------------------------------------


class RunError(Exception):
    pass


def run_once(bin_path: str, suite: str, timeout: float) -> dict:
    if not os.path.isfile(bin_path):
        raise RunError(f"bench binary not found: {bin_path}")
    cmd = [bin_path, suite]
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        raise RunError(f"{bin_path} {suite} timed out after {timeout:.0f}s")
    except OSError as exc:
        raise RunError(f"failed to launch {bin_path}: {exc}")
    out = (proc.stdout or "") + "\n" + (proc.stderr or "")
    if proc.returncode != 0:
        raise RunError(
            f"{bin_path} {suite} exited {proc.returncode}\n{out.strip()[-2000:]}"
        )
    metrics = parse_metrics(out, suite)
    if not metrics:
        raise RunError(
            f"no metrics parsed from {bin_path} {suite} "
            f"(need a 'BENCH {suite} k=v ...' line; see tools/perf/README.md)"
        )
    return metrics


def aggregate(runs: list) -> dict:
    """runs: list of {metric: value} -> {metric: {median, min, max, n}}."""
    keys: set[str] = set()
    for r in runs:
        keys.update(r)
    agg = {}
    for k in sorted(keys):
        vals = [r[k] for r in runs if k in r]
        agg[k] = {
            "median": statistics.median(vals),
            "min": min(vals),
            "max": max(vals),
            "n": len(vals),
        }
    return agg


# --- gating / history --------------------------------------------------------


def check_thresholds(agg: dict, suite: str, thresholds: dict) -> list:
    """thresholds: {suite: {metric: limit}}. Returns [(metric, value, limit)]."""
    limits = thresholds.get(suite, {}) if thresholds else {}
    breaches = []
    for key, limit in limits.items():
        if key.startswith("_"):
            continue
        if key in agg and agg[key]["median"] > float(limit):
            breaches.append((key, agg[key]["median"], float(limit)))
    return breaches


def load_json(path: str, default):
    try:
        with open(path, "r", encoding="utf-8-sig") as fh:  # -sig: tolerate BOM
            return json.load(fh)
    except FileNotFoundError:
        return default


def save_history(path: str, records: list) -> None:
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(records, fh, indent=1)
    os.replace(tmp, path)


def previous_record(records: list, suite: str):
    """Most recent earlier record that contains this suite.

    Called BEFORE the current run is appended, so the last entry is a
    legitimate candidate.
    """
    for rec in reversed(records):
        if suite in rec.get("suites", {}):
            return rec
    return None


# --- output ------------------------------------------------------------------


def fmt_val(key: str, v: float) -> str:
    return f"{v:.0f}" if key in _COUNT_KEYS else f"{v:.3f}"


def print_run_line(idx: int, total: int, metrics: dict) -> None:
    parts = [f"{k}={fmt_val(k, v)}" for k, v in sorted(metrics.items())]
    print(f"  run {idx}/{total}: " + " ".join(parts))


def print_agg(suite: str, agg: dict) -> None:
    parts = [f"{k}={fmt_val(k, a['median'])}" for k, a in sorted(agg.items())]
    print(f"  median [{suite}]: " + " ".join(parts))


def print_diff(suite: str, agg: dict, prev: dict) -> None:
    prev_metrics = prev["suites"][suite]["medians"]
    print(f"  diff vs '{prev.get('label', prev.get('ts', '?'))}' [{suite}]:")
    for k, a in sorted(agg.items()):
        if k not in prev_metrics:
            print(f"    {k}: new ({fmt_val(k, a['median'])})")
            continue
        old = prev_metrics[k]
        if old == 0:
            print(f"    {k}: {fmt_val(k, old)} -> {fmt_val(k, a['median'])}")
            continue
        delta = a["median"] - old
        pct = 100.0 * delta / old
        arrow = "UP" if delta > 0 else ("DOWN" if delta < 0 else "SAME")
        print(
            f"    {k}: {fmt_val(k, old)} -> {fmt_val(k, a['median'])} "
            f"({arrow} {abs(pct):.1f}%)"
        )


# --- selftest ----------------------------------------------------------------

_SYNTH_PRIMARY = (
    "Matted Engine bench - render suite\n"
    "instances=1000000 culled=812345\n"
    "BENCH render p99_ms=7.42 mean_ms=5.10 fps=196.2 instances=1,000,000 draws=1,200\n"
    "frame p99: 7.42 ms (not parsed - primary wins)\n"
)
_SYNTH_FALLBACK = (
    "rendering 500k cubes...\n"
    "frame p99: 12.345 ms\n"
    "mean frame time: 9.876 ms\n"
    "fps 143.2\n"
    "bodies: 10,000 contacts: 45,000 steps = 120\n"
)


def selftest() -> bool:
    checks = []

    def check(name, ok):
        checks.append((name, ok))

    # 1. primary-line parsing (comma counts, multiple keys)
    m = parse_metrics(_SYNTH_PRIMARY, "render")
    check("primary p99_ms", m.get("p99_ms") == 7.42)
    check("primary mean_ms", m.get("mean_ms") == 5.10)
    check("primary fps", m.get("fps") == 196.2)
    check("primary instances comma", m.get("instances") == 1000000.0)
    check("primary draws", m.get("draws") == 1200.0)
    check("primary ignores other suite", "bodies" not in parse_metrics(
        "BENCH physics p99_ms=3.1\n", "render"))

    # 2. fallback parsing (prose output)
    m2 = parse_metrics(_SYNTH_FALLBACK, "render")
    check("fallback p99_ms", abs(m2.get("p99_ms", 0) - 12.345) < 1e-9)
    check("fallback mean_ms", abs(m2.get("mean_ms", 0) - 9.876) < 1e-9)
    check("fallback fps", abs(m2.get("fps", 0) - 143.2) < 1e-9)
    check("fallback bodies", m2.get("bodies") == 10000.0)
    check("fallback contacts", m2.get("contacts") == 45000.0)
    check("fallback steps", m2.get("steps") == 120.0)

    # 3. aggregation (median, outlier-robust)
    agg = aggregate([
        {"p99_ms": 7.0}, {"p99_ms": 7.5}, {"p99_ms": 30.0},
    ])
    check("aggregate median", abs(agg["p99_ms"]["median"] - 7.5) < 1e-9)
    check("aggregate min/max", agg["p99_ms"]["min"] == 7.0
          and agg["p99_ms"]["max"] == 30.0)

    # 4. threshold gate: breach + pass + missing-metric-not-gated
    breaches = check_thresholds(agg, "render", {"render": {"p99_ms": 6.0}})
    check("gate breach", len(breaches) == 1 and breaches[0][0] == "p99_ms")
    check("gate pass", check_thresholds(agg, "render",
                                        {"render": {"p99_ms": 8.0}}) == [])
    check("gate unknown metric skipped",
          check_thresholds(agg, "render", {"render": {"fps": 30.0}}) == [])
    check("gate other suite not applied",
          check_thresholds(agg, "physics", {"render": {"p99_ms": 6.0}}) == [])

    # 5. history diff
    prev = {"label": "old", "suites": {"render": {"medians": {"p99_ms": 6.0}}}}
    recs = [{"label": "old", "suites": prev["suites"]}, {"label": "new"}]
    got = previous_record(recs, "render")
    check("history previous", got is recs[0])

    passed = sum(1 for _, ok in checks if ok)
    for name, ok in checks:
        print(f"  selftest {'PASS' if ok else 'FAIL'}: {name}")
    print(f"selftest: {passed}/{len(checks)} checks passed")
    return passed == len(checks)


# --- main --------------------------------------------------------------------


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Matted Engine perf-regression harness (TASK-19)")
    ap.add_argument("--suite", choices=SUITES + ("both",), default="both")
    ap.add_argument("--bin", default=os.path.join("build", "bench.exe"),
                    help="path to bench binary (default build/bench.exe)")
    ap.add_argument("--runs", type=int, default=3,
                    help="runs per suite, median reported (default 3)")
    ap.add_argument("--thresholds", default=os.path.join("tools", "perf",
                                                         "thresholds.json"))
    ap.add_argument("--history", default=os.path.join("tools", "perf",
                                                      "history.json"))
    ap.add_argument("--label", default="", help="label stored in history")
    ap.add_argument("--check", action="store_true",
                    help="gate medians against thresholds (exit 2 on breach)")
    ap.add_argument("--diff", action="store_true",
                    help="print delta vs the previous history record")
    ap.add_argument("--timeout", type=float, default=300.0,
                    help="per-run timeout seconds (default 300)")
    ap.add_argument("--json", action="store_true", help="print record JSON")
    ap.add_argument("--no-save", action="store_true",
                    help="do not append to history")
    ap.add_argument("--selftest", action="store_true",
                    help="verify parser/gate/history logic and exit")
    args = ap.parse_args(argv)

    if args.selftest:
        return EXIT_OK if selftest() else 1

    if args.runs < 1:
        print("error: --runs must be >= 1", file=sys.stderr)
        return EXIT_RUNFAIL
    suites = list(SUITES) if args.suite == "both" else [args.suite]
    thresholds = load_json(args.thresholds, {})

    record = {
        "ts": time.strftime("%Y-%m-%d %H:%M:%S"),
        "label": args.label,
        "bin": args.bin,
        "runs": args.runs,
        "suites": {},
    }
    failures = []

    for suite in suites:
        print(f"== bench [{suite}] x{args.runs}: {args.bin} ==")
        try:
            run_metrics = []
            for i in range(1, args.runs + 1):
                metrics = run_once(args.bin, suite, args.timeout)
                run_metrics.append(metrics)
                print_run_line(i, args.runs, metrics)
        except RunError as exc:
            print(f"RUN FAILURE [{suite}]: {exc}", file=sys.stderr)
            failures.append(suite)
            continue

        agg = aggregate(run_metrics)
        print_agg(suite, agg)
        breaches = check_thresholds(agg, suite, thresholds)
        if args.diff:
            prev = previous_record(load_json(args.history, []), suite)
            if prev:
                print_diff(suite, agg, prev)

        medians = {k: a["median"] for k, a in agg.items()}
        record["suites"][suite] = {"medians": medians, "agg": agg,
                                   "breaches": [list(b) for b in breaches]}
        if args.check and breaches:
            print(f"  THRESHOLD BREACHES [{suite}]:")
            for key, val, limit in breaches:
                pct = 100.0 * (val - limit) / limit if limit else float("inf")
                print(f"    {suite}.{key} median {fmt_val(key, val)} "
                      f"> limit {fmt_val(key, limit)} (+{pct:.1f}%)")

    if failures:
        print("\n!!!!!! BENCH RUN FAILED !!!!!", file=sys.stderr)
        print(f"  suites failed: {', '.join(failures)}", file=sys.stderr)
        return EXIT_RUNFAIL

    if not record["suites"]:
        print("error: no suites produced metrics", file=sys.stderr)
        return EXIT_PARSE

    if not args.no_save:
        records = load_json(args.history, [])
        records.append(record)
        try:
            save_history(args.history, records)
            print(f"history: {len(records)} record(s) in {args.history}")
        except OSError as exc:
            print(f"warning: could not save history: {exc}", file=sys.stderr)

    if args.json:
        print(json.dumps(record, indent=1))

    if args.check:
        total = sum(len(rec_suites["breaches"])
                    for rec_suites in record["suites"].values())
        if total:
            # stdout (not stderr) so the banner keeps its place under the
            # per-suite breach details even when output is captured/redirected
            print("\n!!!!!! PERF REGRESSION - FAIL !!!!!")
            print(f"  {total} threshold breach(es) - see above")
            print("  action: post to #build per AGENTS.md rule 6")
            return EXIT_BREACH
        print("\nPASS - all gated metrics within thresholds")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
