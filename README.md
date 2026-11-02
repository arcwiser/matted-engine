# Matted Engine

Matted Engine is a performance-first C++20 game-engine project focused on
large scenes, stable frame times, and destruction physics. The codebase is
currently under active development by multiple coordinated agents.

## Current status

Implemented in the current tree:

- core math, logging, arenas and per-thread scratch memory;
- a persistent worker pool with chunked `parallel_for`;
- a generational, dense-storage ECS registry;
- lightweight per-frame CPU profiling;
- mesh, material, camera, LOD, culling, and draw-batch data paths;
- a graphics-API-neutral render-backend interface;
- a tile-based multithreaded software rasterizer;
- a Win32/GDI window and input layer;
- rigid-body physics APIs for broadphase, contacts, solving, sleeping, raycasts,
  and grid fracture.

In progress: runnable demo and benchmark applications, the CMake/MSVC build,
the D3D12 backend, asset loading, and additional convex/destruction features.
See the MattedComms task board for live ownership and status.

## Repository layout

```text
engine/core/             Math, jobs, ECS, memory, logging, profiling
engine/render/           Render data, queue, backend contract, Win32 window
engine/render/software/  Reference CPU rasterizer
engine/physics/          Rigid bodies, collision solving, fracture
tests/                   Zero-dependency C++ tests (AgentB-owned)
tools/mattedcomms/       Local multi-agent coordination service and CLI
docs/                    Architecture and contributor onboarding
```

## Intended build and run workflow

The project targets MSVC 2022 and C++20 with `/O2 /fp:fast /EHsc`. Once the
build-system task lands, the supported commands are:

```bat
scripts\build.bat
build\demo.exe
build\bench.exe render
build\bench.exe physics
```

If `scripts\build.bat` is not present yet, the build task is still in flight;
do not create a competing build system. Coordinate with AgentA in `#build`.

## Design priorities

- Data-oriented hot paths and dense storage.
- No avoidable per-frame heap churn; use `mtt::mem::scratch()` for transient
  frame data.
- Parallel work through `mtt::jobs::parallel_for`.
- Profile before optimizing with `MTT_PROFILE_ZONE` and `prof::report_top`.
- Keep graphics API headers confined to their backend directories.

For module boundaries and frame flow, read
[docs/architecture.md](docs/architecture.md). Agents joining the project must
read [AGENTS.md](AGENTS.md) and [docs/onboarding.md](docs/onboarding.md) before
making changes.
