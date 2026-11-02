# Scene file format (matted-scene v1)

Implemented by `apps/demo/scene_json.h/.cpp` (TASK-18 editor-lite, AgentC).
Zero-dependency JSON reader/writer: `apps/demo/scene_json_value.h/.cpp`.

## Purpose

Persist and restore the demo's full runtime state: camera, physics world
config, procedural mesh declarations, materials, and every render instance
with its linked rigid body. Intended for editor-lite save/load (demo keys
F5/F9), hand-authored scenes, and future tooling.

## Reference resolution

- Meshes and materials are referenced by **name**, never runtime id. Ids are
  load-order dependent; names survive edits and reordering.
- Unknown mesh generators and unknown materials produce a **warning**, not an
  error; the loader keeps going (forward compatibility).
- Mesh geometry is procedural in the demo, so files store *generation specs*,
  not vertex data. The demo declares how each named mesh is built
  (`declare_mesh_source`) right after `setup_meshes()`; `load` re-runs those
  generators through the gen registry (`register_mesh_gen`; the five engine
  generators are pre-registered).

## Top-level schema

```json
{
  "format": "matted-scene",
  "version": 1,
  "camera":   { "pos": [x,y,z], "yaw": rad, "pitch": rad,
                "fov_y_deg": deg, "near_z": f, "far_z": f },
  "world":    { "gravity": [x,y,z], "substeps": u, "velocity_iterations": u,
                "baumgarte": f, "penetration_slop": f, "max_correction_vel": f,
                "rest_vel_threshold": f, "sleep_lin_tol": f, "sleep_ang_tol": f,
                "sleep_time": f,
                "fracture": { "enabled": b, "impulse_base": f,
                              "impulse_growth": f, "max_depth": u,
                              "min_size": f, "splits": u } },
  "meshes":   [ { "name": s, "base": { "gen": s, "params": {} },
                  "lod1": { "gen": s, "params": {} },
                  "lod2": null } ],
  "materials":[ { "name": s, "albedo": [r,g,b], "checker": f,
                  "checker_dim": f } ],
  "instances":[ { "mesh": s, "lod1_dist": f, "lod2_dist": f, "material": s,
                  "pos": [x,y,z], "rot": [x,y,z,w], "scale": [x,y,z],
                  "body": { ... } } ]
}
```

Generator parameters (all optional, defaults shown):

| gen         | params                                     |
|-------------|--------------------------------------------|
| `cube`      | - (24 verts, 12 tris)                      |
| `cube_lod1` | - (8 verts, 12 tris, per-tri face normals) |
| `uv_sphere` | `"segs": 12, "rings": 8`                   |
| `plane_grid`| `"n": 1, "size": 2.0, "repeat": 0`         |
| `quad_xy`   | `"size": 2.0`                              |

The demo's cube declares `lod1: {"gen": "cube_lod1"}` - LOD variants are
separate `MeshLibrary` entries (`add_lod` pushes one); `lod(id, 1..2)` falls
back down the chain to the base mesh.

## Body object

```json
"body": {
  "shape": "sphere | box | plane",
  "radius": 0.5,                    // sphere
  "half": [0.5, 0.5, 0.5],          // box
  "plane_n": [0, 1, 0], "plane_d": 0.0,  // plane (body space)
  "pos": [x,y,z], "rot": [x,y,z,w],
  "mass": 1.0,                      // 0 = static (inv_mass derived on load)
  "lin_vel": [x,y,z], "ang_vel": [x,y,z],
  "friction": 0.6, "restitution": 0.1,
  "static": false, "can_sleep": true, "fracturable": false,
  "depth": 0
}
```

Semantics:

- `mass <= 0` forces the static flag; a positive mass clears it - hand-edited
  files cannot end up with a contradictory state.
- `body.pos/rot` default to the instance-level `pos/rot` when absent.
- Runtime state is **never saved**: sleep timers, world AABBs, `kBodyAsleep`.
  The world recomputes all of it.
- Instances without a `body` object are decorative render slots.
- Instance order = save order = restore order; the demo's ground plane must
  stay first so it remains `BodyId 0` (its reset logic relies on that).

## Restore mechanics (phase-2 consumers)

- `restore_objects()` rebuilds de-duplicated `MeshRef` storage, resolves
  mesh/material ids by name into the *existing* libraries (no duplication),
  and returns parallel `instances / bodies / has_body` arrays. `mesh_ref`
  pointers are only assigned after ref storage stops growing (two-pass).
- `rebuild_meshes()` replaces existing library entries by name in place,
  including LOD slots (stable library size across repeated restores).

## Validation

Round-trip suites (JSON core + scene layer) run green under MSVC 2022
`/std:c++20 /W4 /EHsc` against the real engine headers: 20 checks for the
JSON core (round-trips, unicode + surrogate pairs, error line info, key-order
stability, 200-deep nesting) and ~40 for the scene layer (field fidelity,
mass/flag semantics, name resolution, pointer stability, corrupt/missing file
rejection). Test sources live with AgentC pending a permanent home in
`tests/` (AgentB's area - to be coordinated).
