// Matted Engine - meshes, materials, cameras
#pragma once
#include <array>
#include <string>
#include <vector>
#include "engine/core/math/math.h"
#include "engine/render/mesh.h"

namespace mtt {

// ------------------------------------------------------------------ meshes
struct Vertex {
    vec3 pos;
    vec3 normal;
    vec2 uv;
};

struct MeshData {
    std::vector<Vertex> verts;
    std::vector<u32>    indices;
    aabb                bounds;

    u32 tri_count() const { return (u32)(indices.size() / 3); }
};

using MeshId = u32;

// Mesh library with optional LOD chains (slot 0 = highest detail).
struct MeshLibrary {
    MeshId add(MeshData&& data, const char* name = "");
    // register a lower-detail variant for an existing mesh (slot 1..2)
    void   add_lod(MeshId base, u32 slot, MeshData&& data);
    MeshData&       get(MeshId id)             { return meshes_[id]; }
    const MeshData& get(MeshId id) const       { return meshes_[id]; }
    MeshId          lod(MeshId id, u32 slot) const; // falls back to id
    const char*     name(MeshId id) const { return names_[id].c_str(); }
    size_t          count() const { return meshes_.size(); }

    std::vector<MeshData> meshes_;
    std::vector<std::string> names_;
    std::vector<std::array<MeshId, 3>> lods_; // slot0=base, slot1/2 = lower LODs (or same id)
};

// LOD distance thresholds (metres) applied when picking a mesh variant.
struct LodSettings {
    float lod1_dist = 35.0f;
    float lod2_dist = 90.0f;
};

// Mesh generators
namespace meshgen {
MeshData cube();                          // 24 verts, 12 tris, smooth-face normals
MeshData cube_lod1();                     // 8 verts, 12 tris, per-tri face normals
MeshData uv_sphere(int segs = 12, int rings = 8);
MeshData plane_grid(int n = 1, float size = 2.0f, u32 repeat = 0); // XZ plane
MeshData quad_xy(float size = 2.0f);
} // namespace meshgen

// ------------------------------------------------------------------ materials
using MaterialId = u32;

struct Material {
    vec3  albedo{0.75f, 0.75f, 0.78f};
    float checker = 0.0f;   // 0 = off, else uv repeat count for a 2-tone checker
    float checker_dim = 0.62f; // second tone multiplier
};

struct MaterialLibrary {
    MaterialId add(Material&& m, const char* name = "") {
        materials_.push_back(std::move(m));
        names_.emplace_back(name);
        return (MaterialId)(materials_.size() - 1);
    }
    Material&       get(MaterialId id)       { return materials_[id]; }
    const Material& get(MaterialId id) const { return materials_[id]; }
    std::vector<Material> materials_;
    std::vector<std::string> names_;
};

// ------------------------------------------------------------------ camera
struct Camera {
    vec3  pos{0, 2, 6};
    float yaw = 0;      // radians, 0 looks down -Z
    float pitch = 0;    // radians
    float fov_y = 60.0f * kDegToRad;
    float near_z = 0.1f;
    float far_z = 500.0f;

    vec3 forward() const {
        const float cy = std::cos(yaw), sy = std::sin(yaw);
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        return { -sy * cp, sp, -cy * cp }; // yaw around +Y, pitch after
    }
    vec3 right() const {
        const float cy = std::cos(yaw), sy = std::sin(yaw);
        return { cy, 0, -sy };
    }
    vec3 up() const { return normalize(cross(right(), forward())); }

    mat4 view() const {
        return mat4::look_at(pos, pos + forward(), vec3(0, 1, 0));
    }
    mat4 proj(float aspect) const {
        return mat4::perspective(fov_y, aspect, near_z, far_z);
    }
    mat4 view_proj(float aspect) const { return mat4::mul(proj(aspect), view()); }
};

} // namespace mtt
