# tools/perf — Perf-Regression Harness (TASK-19)

Companion tooling for the engine benchmarks. Stdlib-only Python, no deps,
nothing here touches engine/apps/tests code.

| File | What it does |
|---|---|
| `bench.py` | Runs `build/bench.exe render|physics` N times, parses metrics, keeps `history.json`, gates medians against `thresholds.json`, fails loudly on regression |
| `prof_report.py` | Aggregates `mtt::prof::report_top` dump lines into a sorted top-N cost table |
| `thresholds.json` | Upper limits per suite/metric (median across runs); missing metrics are not gated |
| `history.json` | Append-only run history (created on first real run) |
| `selftest/` | Sample outputs + fake-binary fixtures |

## bench.py

```bat
:: standard gate (both suites, 3 runs each, thresholds checked)
python tools\perf\bench.py --suite both --check

:: more runs + label stored in history
python tools\perf\bench.py --suite render --runs 5 --label "tile sweep 32"

:: compare against the previous recorded run
python tools\perf\bench.py --suite physics --diff

:: machine-readable record
python tools\perf\bench.py --suite both --check --json

:: verify the tool itself (no bench.exe needed)
python tools\perf\bench.py --selftest
```

Exit codes: **0** pass · **2** threshold breach · **3** binary failed/missing ·
**4** no metrics parsed. A `2` should be posted to `#build` per AGENTS.md rule 6.

### Expected bench output format

Primary parse target — one summary line per suite:

```text
BENCH render   p99_ms=7.42 mean_ms=5.10 fps=196.2 instances=1000000 draws=1200
BENCH physics  p99_ms=3.10 mean_ms=2.20 bodies=10000 contacts=45000 steps=120
```

If no `BENCH <suite> k=v` line is found, tolerant fallback regexes kick in
(`p99 ... 12.3 ms`, `fps 143.2`, `bodies: 10,000`, ...). Metrics recognized by
fallback: `p99_ms mean_ms avg_ms min_ms max_ms fps instances draws culled
bodies contacts steps frames`. Known metrics from the primary line are never
limited to that list.

### Aggregation and gating

- Each suite runs `--runs` times (default 3); the **median** of every metric
  is reported and stored (robust against one noisy run).
- `--check` compares each gated metric's median to its limit in
  `thresholds.json`; any median **above** the limit is a breach → exit 2.
- Every invocation appends a record to `history.json` unless `--no-save`;
  `--diff` prints deltas vs the previous record containing that suite.

### thresholds.json

```json
{
  "_doc": ["..."],
  "render":  { "p99_ms": 16.7, "mean_ms": 12.0 },
  "physics": { "p99_ms": 10.0, "mean_ms": 8.0 }
}
```

Current values are placeholders until the first real `bench.exe` numbers
exist (AgentA's TASK-7). Tighten them afterwards.

## prof_report.py

```bat
:: from a log file
python tools\perf\prof_report.py bench_log.txt --top 10

:: from a pipe
build\demo.exe | python tools\perf\prof_report.py

:: sort by call count instead of ms; machine-readable
python tools\perf\prof_report.py bench_log.txt --sort calls --json
```

Parses lines shaped `  prof <name> <ms> ms (<calls> calls)` (the
`prof::report_top` format in `engine/core/profiler.cpp`), sums every zone
across all snapshots in the input, and reports total ms, avg ms per snapshot,
calls, avg calls per snapshot, and %-share of total measured time.

## Quick verification without the real bench.exe

```bat
python tools\perf\bench.py --selftest
python tools\perf\prof_report.py tools\perf\selftest\sample_prof_log.txt
```
