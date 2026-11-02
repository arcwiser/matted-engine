# Matted Engine - Agent Coordination (AGENTS.md)

Two AI agents are building this engine together. This file is the contract.
**Read this fully before touching any code.**

## The team

| Agent   | Focus                                  | Chat user |
|---------|----------------------------------------|-----------|
| AgentA  | Engine core, render queue, software rasterizer, physics/solver core, build system | `AgentA` |
| AgentB  | GPU backend (D3D12/Vulkan), asset pipeline (glTF), extra physics (GJK/EPA hulls, joints), tools/tests | `AgentB` |

AgentA is currently mid-flight on: render queue (`engine/render/render_queue.cpp`),
software rasterizer backend, physics world/solver, demo + bench apps, CMake build.

## Idle protocol: never sit around, create your own tasks

If `task list` shows nothing unclaimed that fits you:

1. Invent the next most valuable task, consistent with the mission
   (performance-first rendering, large-scale destruction physics) and AGENTS.md conventions.
2. Add it with the next free number:
   `task add "TASK-<n> [P0|P1|P2][area] <what must be done>. Gate: <measurable done-check>"`.
   P0 = blocks shipping, P1 = important, P2 = polish.
3. Announce it in `#tasks`, claim it (`task claim --id N --user <name>`), and start.
4. Never take a task another agent has claimed. Prefer P0 work. If your new task
   would touch another agent's files, propose it in `#general` first.

## Onboarding new agents (for the human user)

To bring a new agent online: run `scripts\onboard.cmd`, copy its output, and
paste it into the new agent. The agent will then run `mattedcomms.py join`,
which posts its arrival, prints the digest, and lists unclaimed work.

Check team health any time with:

    python tools\mattedcomms\mattedcomms.py status

## Constant background duty (mandatory for every agent)

Every agent MUST keep this running in a background shell for the whole session:

    python tools\mattedcomms\mattedcomms.py watch --user <YourName>

The watcher heartbeats presence, streams incoming chat, posts a periodic
`[auto] <name> online` ping to #general, and recovers the server if it dies.
It is how `status` (and the human) knows you are alive. If you go dark for
more than ~5 minutes without notice, another agent may reassign your tasks.

## Communication: MattedComms (local chat, already running)

Run commands from the repo root: `C:\Users\Admin\Desktop\matted engine`

```bat
:: check for messages addressed to you (prints new ones, remembers your cursor)
python tools\mattedcomms\mattedcomms.py recv --user AgentB

:: send
python tools\mattedcomms\mattedcomms.py send --user AgentB --channel "#general" "your message"

:: live stream (blocks; use in a background shell if you want)
python tools\mattedcomms\mattedcomms.py recv --user AgentB --follow

:: task board
python tools\mattedcomms\mattedcomms.py task list
python tools\mattedcomms\mattedcomms.py task claim --id 2
python tools\mattedcomms\mattedcomms.py task done --id 2
```

Rules of the road:

1. **Check `recv` when you start and every few minutes of work.** Reply to direct
   questions in `#general` within the same session.
2. Any message starting with `@YourName` requires a reply in `#general` within
   5 minutes, even if the answer is "still working, no blockers". Silence makes
   you look dead; dead agents' tasks get reassigned.
3. **Cursor rule:** `watch` (no flags) shares the per-user read cursor with
   foreground `recv` — never run both for the same user at once. Run your
   watcher as `watch --user X --no-cursor` if you also want ad-hoc `recv`.
4. Channels: `#general` (coordination), `#rendering`, `#physics`, `#tasks`
   (task hand-offs), `#build` (build breakages).
5. **Claim a task before starting it** (`task claim --id N`) and say so in `#tasks`.
6. If you break the build, post in `#build` immediately with the error.

## Shared ownership rules

- AgentA owns: `engine/core/**`, `engine/render/render_queue.*`,
  `engine/render/software/**`, `engine/physics/**` (world/solver/broadphase),
  `apps/**`, `CMakeLists.txt`, `scripts/**`.
- AgentB owns: `engine/render/d3d12/**` (new), `engine/render/vulkan/**` (new),
  `engine/assets/**` (new), `engine/physics/gjk/**` + `fracture_voronoi.*` (new),
  `tests/**` (new).
- Anything else, or edits inside the other agent's area: **propose in `#general`
  first and wait for an ack.** Never force-push over a file the other agent just
  changed; coordinate in chat.

## Code conventions

- Language: **C++20**, compiled with MSVC 2022 (`/O2 /fp:fast /EHsc`).
- Single namespace `mtt` (sub-namespaces: `mtt::jobs`, `mtt::ecs`, `mtt::mem`,
  `mtt::prof`, `mtt::physics`).
- Headers `#pragma once`; includes ordered: own header, engine, std.
- Performance style: data-oriented, SoA where hot, no per-frame heap churn
  (use `mtt::mem::scratch()` arenas), jobify with `mtt::jobs::parallel_for`.
- Profile before optimizing: `MTT_PROFILE_ZONE("name")` + `prof::report_top`.

## Git: push every change (MANDATORY)

Repo: https://github.com/arcwiser/matted-engine (branch: `main`)

WHEN to commit+push:
- immediately after every completed, compiling work unit (a fix, a feature, a task milestone)
- before switching to another task
- right after you fix a broken build
- at minimum: every ~30 minutes of active coding

HOW (use the helper):

    scripts\push.bat "[AgentX][TASK-N] one-line summary of what changed"

That stages everything, commits, and pushes to `origin main`. Equivalent manual
sequence: `git add -A` -> `git commit -m "..."` -> `git push`.

RULES:
1. NEVER force-push to `main`. If your push is rejected: `git pull --rebase`,
   resolve, push again. Cross-agent conflicts -> resolve in `#general`, not in silence.
2. `build/`, `.comms/`, and generated binaries are git-ignored - never add them by hand.
3. Only push code that compiles (`scripts\build.bat` green). If it cannot compile,
   push anyway but START the commit message with `[WIP-BROKEN]` and post in `#build`.
4. Commit messages: `[AgentX][TASK-N] what changed` - the board + git history must
   line up so any agent (or the human) can audit work.
5. Do not edit another agent's committed files without the chat protocol from
   "Shared ownership rules" - git history makes every violation visible.

## Build

```bat
scripts\build.bat          :: CMake + Ninja + MSVC Release -> build\demo.exe, build\bench.exe
build\demo.exe             :: interactive window (WASD + RMB look, LMB shoot, F fracture, R reset)
build\bench.exe render     :: headless massive-scene render benchmark
build\bench.exe physics    :: headless destruction physics benchmark
```

If `scripts\build.bat` fails on your machine, tell AgentA in `#build` — do not
replace the build system.
