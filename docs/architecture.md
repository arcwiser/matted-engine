# Matted Engine architecture

This document describes the architecture visible in the current source tree.
It is a guide to boundaries and data flow, not a promise that every application
and backend task is already complete.

## Frame-level data flow

```text
game/ECS state
    |
    v
RenderInstance span + Camera
    |
    v
RenderQueue::build
  1. parallel frustum cull
  2. distance-based LOD selection
  3. group by (MeshId, MaterialId)
  4. parallel model-matrix bake
    |
    v
FramePacket { BakedInstance[], DrawBatch[] }
    |
    v
IRenderBackend
  begin_frame -> draw_batch* -> end_frame
    |
    +--> SoftwareBackend: BGRA framebuffer -> Win32/GDI Window
    `--> GPU backend: API-specific presentation (in progress)
```

Batch ordering across distinct mesh/material keys is unspecified. Consumers
must use each `DrawBatch`'s mesh, material, `first`, and `count` rather than
depending on a global key order.

## Core (`engine/core`)

- **Types and math:** fixed-width aliases, entity identifiers, vectors,
  quaternions, matrices, bounds, and frusta.
- **Jobs:** a persistent worker pool. `parallel_for` splits a range into chunks
  and lets the calling thread participate. Nested worker calls fall back to
  inline execution.
- **Memory:** a chained monotonic `Arena` and thread-local scratch arena. Arena
  reset is bulk reclamation; callers do not individually free arena objects.
- **ECS:** generational entity handles prevent stale-handle aliasing. Component
  pools use dense arrays and swap-removal, with a sparse index for lookup.
- **Profiler:** scoped zones aggregate CPU timings per frame and expose counts,
  average milliseconds, and top-zone reporting.

## Rendering (`engine/render`)

`MeshLibrary` owns mesh data and optional LOD chains. `MaterialLibrary` owns
simple material records. `Camera` produces view and projection matrices.

`RenderQueue` converts scene instances into a backend-ready `FramePacket`.
Visibility and matrix baking are jobified. The queue needs stable `MeshRef`
storage because each `RenderInstance` holds a pointer to a `MeshRef`.

`IRenderBackend` is deliberately graphics-API-neutral. Implementations receive
complete mesh/material batches and expose per-frame `BackendStats`. Software
backends may expose packed BGRA8888 pixels; GPU backends may return `nullptr`
from `pixels()` and own presentation themselves.

`SoftwareBackend` is the reference implementation. It transforms geometry,
bins instances into 32x32-pixel tiles, then rasterizes independent tiles in
parallel with depth testing, perspective-correct UVs, and Lambert lighting.

The Win32 `Window` contains OS-specific window, input, framebuffer blit, and HUD
behavior. Win32 details stay in its implementation file.

## Physics (`engine/physics`)

`mtt::physics::World` owns bodies in dense vectors and recycles body IDs. Its
configured pipeline is:

1. integrate forces and velocities;
2. incremental X-axis sweep-and-prune broadphase;
3. analytic sphere/box/plane and SAT box-box narrowphase;
4. persistent manifolds and warm-started sequential impulses;
5. position integration, island processing, and sleeping;
6. deferred fracture processing.

Contacts keep accumulated normal and tangent impulses for warm starting.
Fracture is impulse-threshold driven, depth limited, and drained from a queue at
the end of a step so body creation/removal does not invalidate active solver
work. Convex GJK/EPA and Voronoi fracture are separate in-progress extensions.

## Ownership boundaries

Ownership is authoritative in `AGENTS.md`. In summary, AgentA owns the engine
core, render queue/software backend, base physics world, apps, and build system;
AgentB owns GPU backends, assets, additional convex/fracture physics, and tests.
AgentC currently owns the documentation task only. Coordinate in `#general`
before editing outside an assigned area.

## Performance invariants

- Prefer dense iteration and structure layouts suitable for hot loops.
- Reuse transient storage and avoid per-frame allocation churn.
- Choose job grain sizes large enough to amortize scheduling overhead.
- Preserve stable handles or explicitly documented pointer lifetimes.
- Add profiling zones before making performance claims.
- Keep backend-specific dependencies behind `IRenderBackend`.
