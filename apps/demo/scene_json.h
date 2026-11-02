// Matted Engine - JSON scene save/load for the demo (TASK-18 editor-lite).
//
// Serializes the demo's actual runtime state (physics::World bodies +
// RenderInstance list + libraries + camera), NOT an ECS: the demo app is
// hand-rolled SoA (see apps/demo/main.cpp). One file = one restorable scene.
//
// Schema v1 ("matted-scene"):
// {
//   "format": "matted-scene", "version": 1,
//   "camera": { "pos":[x,y,z], "yaw":rad, "pitch":rad,
//               "fov_y_deg":deg, "near_z":f, "far_z":f },
//   "world":  { ...physics::WorldConfig fields..., "fracture": {...} },
//   "meshes": [ { "name":libname, "base":{ "gen":s, "params":{...} },
//                 "lod1":{...}|null, "lod2":{...}|null } ],
//   "materials": [ { "name":s, "albedo":[r,g,b], "checker":f,
//                    "checker_dim":f } ],
//   "instances": [ { "mesh":s, "lod1_dist":f, "lod2_dist":f, "material":s,
//                    "pos":[x,y,z], "rot":[x,y,z,w], "scale":[x,y,z],
//                    "body": { "shape":"sphere|box|plane", "radius":f,
//                              "half":[...], "plane_n":[...], "plane_d":f,
//                              "mass":f, "lin_vel":[...], "ang_vel":[...],
//                              "friction":f, "restitution":f,
//                              "static":b, "can_sleep":b, "fracturable":b,
//                              "depth":u } | absent } ]
//
// Design rules:
//  - Meshes/materials are referenced by NAME, never runtime id (ids are
//    load-order dependent; names survive edits).
//  - Mesh geometry is procedural in the demo, so files store generation
//    specs, not vertex data. Apps declare how each named mesh is built
//    (declare_mesh_source) right after setup; load re-runs those generators
//    through the gen registry (register_mesh_gen; built-ins auto-registered).
//  - Body mass is stored as mass (0 = static, human-editable); inv_mass and
//    the static flag are derived on load so hand-edited files stay coherent.
//    Runtime-derived state (sleep timer, world AABBs, kBodyAsleep) is never
//    saved - the world recomputes it.
//  - Unknown mesh gens / materials are skipped with a warning, not an error:
//    forward compatibility.
//  - Save/load are cold-path operations; std::string/std::vector storage is
//    fine per engine conventions (no per-frame heap churn here).
#pragma once
#include "scene_json_value.h"

#include "engine/core/math/math.h"
#include "engine/physics/physics.h"
#include "engine/render/mesh.h"
#include "engine/render/render_queue.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mtt::scene {

// ------------------------------------------------------------ mesh gens
// Factory for one procedural mesh; params come from the file.
using MeshGenFn = std::function<MeshData(const json::Value& params)>;

// Registers (or replaces) a generator by gen name. The five engine
// generators (cube, cube_lod1, uv_sphere, plane_grid, quad_xy) are
// pre-registered; apps only need this for custom gens.
void register_mesh_gen(const char* gen_name, MeshGenFn fn);
const MeshGenFn* find_mesh_gen(const char* gen_name);

// How one MeshLibrary entry is produced: base mesh + optional LOD slots.
struct MeshDecl {
    std::string gen;
    json::Value params = json::Value::object();
};
struct MeshSource {
    std::string lib_name;
    MeshDecl base;
    MeshDecl lod1; bool has_lod1 = false;   // slot 1
    MeshDecl lod2; bool has_lod2 = false;   // slot 2
};

// App declares, once after building its MeshLibrary, how each named mesh is
// generated. save_scene writes every declaration; load_scene returns them in
// SceneSave::meshes. Overwrites any previous declaration for lib_name.
void       declare_mesh_source(const MeshSource& src);
const MeshSource* find_mesh_source(const char* lib_name);
void       clear_mesh_sources();

// Convenience: declare the standard engine generators for `lib_name`.
void declare_builtin_mesh(const char* lib_name, const char* gen,
                          json::Value params = json::Value::object());

// ------------------------------------------------------------ snapshots
struct InstanceSave {
    // render
    std::string mesh_name;              // MeshLibrary name
    LodSettings lod;
    std::string material;               // MaterialLibrary name
    vec3 pos{0, 0, 0};
    quat rot = quat::identity();
    vec3 scale{1, 1, 1};
    // linked physics body (demo binds 1:1; absent for freed slots - those
    // are simply not saved)
    bool has_body = false;
    physics::Body body;
};

struct SceneSave {
    Camera camera;
    physics::WorldConfig world;
    std::vector<MeshSource> meshes;                                // from declarations
    std::vector<std::pair<std::string, Material>> materials;       // name + material
    std::vector<InstanceSave> instances;                           // save/load order
};

// ------------------------------------------------------------ file api
bool save_scene(const SceneSave& scene, const char* path, std::string* err = nullptr);
bool load_scene(const char* path, SceneSave& scene, std::string* err = nullptr,
                std::vector<std::string>* warnings = nullptr);
bool load_scene_from_memory(const char* text, size_t len, SceneSave& scene,
                            std::string* err = nullptr,
                            std::vector<std::string>* warnings = nullptr);

// ------------------------------------------------------------ restore helpers
// Rebuilds a MeshLibrary from saved declarations. Existing entries with the
// same name are replaced in place (vectors are public in MeshLibrary); gens
// that are unknown produce a warning and an empty mesh so references resolve.
void rebuild_meshes(const std::vector<MeshSource>& sources, MeshLibrary& lib,
                    std::vector<std::string>* warnings = nullptr);

// Rebuilds demo-side render + body objects from a loaded SceneSave.
//  - refs/instances: parallel arrays; instance i uses refs[instances[i].mesh_ref]
//  - bodies[i]: valid when has_body, else default (demo decides spawn policy)
//  - mesh ids resolve by name against `lib` (missing -> warning + empty mesh
//    registered under that name); material ids resolve by name against `mats`
//    (missing -> 0 + warning)
//  - refs are de-duplicated by (mesh, lod1_dist, lod2_dist) to match the
//    demo's small stable ref-storage pattern
struct RestoredScene {
    std::vector<MeshRef> refs;
    std::vector<RenderInstance> instances;
    std::vector<physics::Body> bodies;      // parallel to instances
    std::vector<bool> has_body;             // parallel to instances
};
bool restore_objects(const SceneSave& in, MeshLibrary& lib, MaterialLibrary& mats,
                     RestoredScene& out, std::vector<std::string>* warnings = nullptr);

// Convenience: build one InstanceSave from demo-side data (phase-2 hook the
// demo calls per bound instance; body may be nullptr for decorative slots).
InstanceSave make_instance_save(const RenderInstance& ri, const physics::Body* body,
                                const MeshLibrary& meshes);

} // namespace mtt::scene
