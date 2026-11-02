// Matted Engine - render queue: culling, LOD selection, batch building.
// Produces a FramePacket the backend consumes. All heavy stages are jobified.
#pragma once
#include <span>
#include <vector>
#include "engine/core/math/math.h"
#include "engine/render/mesh.h"

namespace mtt {

struct MeshRef; // stable mesh+LOD storage, defined below

struct RenderInstance {
    Entity      entity{};
    vec3        pos;
    quat        rot;
    vec3        scale{1, 1, 1};
    MeshRef*    mesh_ref = nullptr;   // pointer into stable MeshRef storage (see below)
    MaterialId  material = 0;
};

// Mesh + LOD distances, stored stably so the queue can read them per frame.
struct MeshRef {
    MeshId      mesh = 0;
    LodSettings lod;
};

struct BakedInstance {
    mat4 model;
};

struct DrawBatch {
    MeshId      mesh;
    MaterialId  material;
    u32         first;    // index into FramePacket::instances
    u32         count;
};

struct FramePacket {
    std::vector<BakedInstance> instances;  // visible, batch-sorted
    std::vector<DrawBatch>     batches;
    u32 instances_total = 0;
    u32 instances_culled = 0;
    u32 lods_selected[3] = {0, 0, 0};
};

class RenderQueue {
public:
    // MeshLibrary is needed for bounds + LOD resolution; must be set before build.
    void set_mesh_library(MeshLibrary* lib) { meshes_ = lib; }

    // Builds the visible, sorted, batched packet for this frame.
    FramePacket build(const Camera& cam, float aspect,
                      std::span<const RenderInstance> instances);

    u64 last_cull_ns = 0;
    u64 last_build_ns = 0;

private:
    MeshLibrary* meshes_ = nullptr;
};

} // namespace mtt
