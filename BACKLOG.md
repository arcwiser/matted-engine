# Matted Engine - Task Stockpile (BACKLOG.md)

Pre-scoped, ready-to-claim work so a newly onboarded agent never has to wait
or invent scope. Each entry below is the **detailed spec** for the matching
board task (`task list`). Claim on the board, announce in `#tasks`, and go.

Rules: AGENTS.md ownership applies — these lanes were pre-cleared by AgentB
(tools/, assets/ data, tests/ under review, docs/ with AgentC). If a task
needs another agent's files, that's called out — get the ack first.

---

## TASK-B1 [P1][tools] Asset validator (supports TASK-2)

`tools/assets/validate_assets.py` — Python 3 stdlib ONLY (json, struct, sys,
pathlib; no pip).

- Modes: `validate <file.glb|.gltf|.obj>` and `validate --manifest assets/test/manifest.json`.
- glTF checks: JSON parses; accessors referenced by attributes exist; index
  buffer values < vertex count; POSITION/NORMAL/TEXCOORD_0 present on meshes;
  no NaN in POSITION min/max; GLB container: magic `glTF`, version 2, chunk
  lengths consistent with file size.
- OBJ checks: `f` indices within declared `v` count; warn on missing `vn`/`vt`;
  faces are triangles (error if not, with line number).
- Exit codes: 0 valid, 1 invalid, 2 usage error. One line per problem:
  `file:line-or-path: SEVERITY: message`.

**Gate:** runs green on 3 fixtures you create under `tools/assets/fixtures/`,
including one deliberately corrupt file per format (bad index, truncated GLB).
**Ping:** AgentB (consumer: TASK-2 loader, AgentB).

## TASK-B2 [P1][assets] Test asset pack

`assets/test/` + `assets/test/manifest.json` — engine code is OFF-LIMITS;
you are producing DATA files only.

- 5 models, each < 2 MB: `cube.glb` (trivial), `capsule.gltf` (medium,
  ~1-5k tris), `torusknot.gltf` (high-ish, 5-20k tris), `textured_cube.obj`
  (+ `.mtl`, uses only paths, no external textures required), `hierarchy.gltf`
  (2+ node transforms, tests node composition).
- `manifest.json`: array of `{ "file", "expected_tris", "expected_verts",
  "source", "license" }` (CC0/CC-BY or self-authored only; note source).
- All indices/attributes follow what `engine/render/mesh.h` expects to load
  (triangles, position+normal+uv).

**Gate:** `validate_assets.py --manifest assets/test/manifest.json` passes;
tri counts in manifest match reality.
**Ping:** AgentB before adding files (engine/assets is AgentB's lane; data
files are yours once acked).

## TASK-B3 [P2][tools] BMP diff tool (render regression harness)

`tools/img/bmp_diff.py` — stdlib only.

- `bmp_diff.py a.bmp b.bmp [--heatmap out.bmp] [--thresh 0.02]`
- Compares 24/32-bit BMPs from `SoftwareBackend::save_bmp` (bench app dumps):
  reports % pixels differing beyond threshold, mean abs channel diff, max
  diff; writes a magenta-highlight heatmap when requested.
- Different sizes => error, not crash.

**Gate:** on two synthetic BMPs you generate in the script's self-test
(`--selftest`), reported metrics match hand-computed values.
**Ping:** AgentA when bench.exe lands (consumer), AgentB for API questions.

## TASK-B4 [P2][tools] File-ownership checker

`tools/dev/file_owner.py <path> [<path>...]` — maps paths to owning agent per
AGENTS.md (AgentA/AgentB/AgentC/shared), exit 1 + reason if a path is
another agent's area. Keep the ownership table in ONE dict at the top with a
comment "keep in sync with AGENTS.md".

**Gate:** correct verdicts on 10 sample paths covering both agents' areas,
shared files, and unknown paths.
**Ping:** AgentA (AGENTS.md is the source of truth).

## TASK-B5 [P0][ci] Repo hygiene: git + .gitignore

**NEEDS explicit ack from AgentA AND the user in #general BEFORE running**
(it initializes version control for the whole repo).

- `git init` (default branch `main`), `.gitignore`: `build/`, `.comms/`,
  `*.obj`, `*.pdb`, `.vs/`, `__pycache__/`; keep `assets/test/` tracked.
- Initial commit "initial engine state", then tag `v0.0.0-stockpile`.

**Gate:** fresh `git status` is clean after `scripts\build.bat` (build output
ignored); log shows exactly one commit + tag.
**Ping:** AgentA + user in #general for the ack; do the work after, not before.

## TASK-B6 [P1][tests] LOD fallback test suite (delegated to tests/)

`tests/test_render_lods.cpp` — extends the AgentB-owned test framework
(`tests/test_framework.h`; read it first, follow its macros).

- Matrix: MeshLibrary with base only / base+lod1 / base+lod1+lod2;
  `lod(id, slot)` for every slot combination; unregistered slot falls back
  correctly; `add_lod` with invalid slot is a no-op (count unchanged).
- **Do not edit existing test files** — new file + one line added to
  `tests/build_tests.bat` source list.

**Gate:** `tests\build_tests.bat` builds and your file reports all-OK
(existing tests untouched and still green).
**Ping:** AgentB (review + merge).

## TASK-B7 [P2][docs] Benchmark user guide

`docs/bench-guide.md` (AgentC owns docs/ — coordinate in #general first,
then write under AgentC's review).

- How to run `bench.exe render|physics`, what each output line means
  (BackendStats fields, `prof::report_top` table), how to capture a BMP dump,
  how to diff two runs with TASK-B3's tool.
- Written from reading the code; every command you document must be one you
  actually ran.

**Gate:** steps reproducible by AgentC; AgentC signs off in #general.
**Ping:** AgentC (owner), AgentA for bench internals.

## TASK-B8 [P1][tools] Comms daily digest

`tools/comms/daily_digest.py --since YYYY-MM-DD` — stdlib only, reads
`.comms/chat.db` read-only (open with `mode=ro`), prints per-channel message
counts, task state changes (needs a simple ts-based diff of the tasks table
snapshot if history is absent — document the limitation), and @mentions per
agent. MUST NOT write to the DB or cursor files.

**Gate:** runs against the live chat.db without moving any read cursor
(verify `state_*.json` unchanged before/after).
**Ping:** AgentB (MattedComms maintainer).

