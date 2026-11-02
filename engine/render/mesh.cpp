#include "engine/render/mesh.h"

namespace mtt {

// ------------------------------------------------------------------ MeshLibrary
MeshId MeshLibrary::add(MeshData&& data, const char* name) {
    meshes_.push_back(std::move(data));
    names_.emplace_back(name ? name : "");
    std::array<MeshId, 3> lod{ (MeshId)(meshes_.size() - 1), kInvalid32, kInvalid32 };
    lods_.push_back(lod);
    return (MeshId)(meshes_.size() - 1);
}
void MeshLibrary::add_lod(MeshId base, u32 slot, MeshData&& data) {
    if (slot == 0 || slot > 2) return;
    meshes_.push_back(std::move(data));
    names_.emplace_back(names_[base] + "_lod" + std::to_string(slot));
    lods_[base][slot] = (MeshId)(meshes_.size() - 1);
}
MeshId MeshLibrary::lod(MeshId id, u32 slot) const {
    const auto& l = lods_[id];
    if (slot > 2) slot = 2; // clamp (AgentB's test note)
    for (u32 s = slot; s > 0; --s)
        if (l[s] != kInvalid32) return l[s];
    return l[0];
}

// ------------------------------------------------------------------ generators
namespace meshgen {

MeshData cube() {
    // 6 faces x 4 verts; CCW when viewed from outside
    static const vec3 P[8] = {
        {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
        {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1},
    };
    static const u32 F[6][4] = {
        {0,1,2,3}, // -Z
        {5,4,7,6}, // +Z
        {4,0,3,7}, // -X
        {1,5,6,2}, // +X
        {4,5,1,0}, // -Y
        {3,2,6,7}, // +Y
    };
    static const vec3 N[6] = {
        {0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},
    };
    MeshData m;
    m.verts.reserve(24);
    for (int f = 0; f < 6; ++f) {
        for (int v = 0; v < 4; ++v) {
            const u32 pi = F[f][v];
            vec2 uv((v == 1 || v == 2) ? 1.f : 0.f, (v >= 2) ? 1.f : 0.f);
            m.verts.push_back({P[pi], N[f], uv});
        }
        const u32 b = (u32)(f * 4);
        m.indices.insert(m.indices.end(), { b, b + 1, b + 2, b, b + 2, b + 3 });
    }
    for (const auto& v : m.verts) m.bounds.expand_point(v.pos);
    return m;
}

MeshData cube_lod1() {
    // 8 shared verts, 12 tris; normals computed per-tri at runtime (flat)
    static const vec3 P[8] = {
        {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
        {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1},
    };
    static const u32 T[12][3] = {
        {0,2,1},{0,3,2}, {4,5,6},{4,6,7}, {4,7,3},{4,3,0},
        {1,6,5},{1,2,6}, {4,1,5},{4,0,1}, {3,6,2},{3,7,6},
    };
    MeshData m;
    m.verts.reserve(8);
    for (const auto& p : P) m.verts.push_back({p, vec3(0), vec2(0)});
    for (const auto& t : T) m.indices.insert(m.indices.end(), { t[0], t[1], t[2] });
    for (const auto& v : m.verts) m.bounds.expand_point(v.pos);
    return m;
}

MeshData uv_sphere(int segs, int rings) {
    MeshData m;
    const float kRep = 1.0f; // uv scale
    for (int r = 0; r <= rings; ++r) {
        const float v = (float)r / rings;
        const float phi = v * kPi;
        for (int s = 0; s <= segs; ++s) {
            const float u = (float)s / segs;
            const float theta = u * kTwoPi;
            const float sp = std::sin(phi), cp = std::cos(phi);
            const float st = std::sin(theta), ct = std::cos(theta);
            vec3 n(sp * ct, cp, sp * st);
            m.verts.push_back({ n, n, vec2(u * kRep, v * kRep) });
        }
    }
    const int row = segs + 1;
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < segs; ++s) {
            const u32 a = (u32)(r * row + s);
            const u32 b = (u32)(a + 1);
            const u32 c = (u32)(a + row);
            const u32 d = (u32)(c + 1);
            m.indices.insert(m.indices.end(), { a, c, b, b, c, d });
        }
    for (const auto& vtx : m.verts) m.bounds.expand_point(vtx.pos);
    return m;
}

MeshData plane_grid(int n, float size, u32 repeat) {
    // XZ plane centred at origin, subdivided nxn, facing +Y
    MeshData m;
    const float step = size / (float)n;
    const float half = size * 0.5f;
    const float uv_step = repeat > 0 ? 1.0f / (float)repeat : 1.0f / (float)n;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x) {
            m.verts.push_back({ {-half + x * step, 0, -half + z * step},
                                {0, 1, 0},
                                {x * uv_step, z * uv_step} });
        }
    const int row = n + 1;
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            const u32 a = (u32)(z * row + x);
            const u32 b = (u32)(a + 1);
            const u32 c = (u32)(a + row);
            const u32 d = (u32)(c + 1);
            // CCW from above (+Y normal): (a,d,c) and (a,b,d)
            m.indices.insert(m.indices.end(), { a, d, c, a, b, d });
        }
    for (const auto& vtx : m.verts) m.bounds.expand_point(vtx.pos);
    return m;
}

MeshData quad_xy(float size) {
    const float h = size * 0.5f;
    MeshData m;
    m.verts = {
        {{-h,-h,0},{0,0,1},{0,0}}, {{h,-h,0},{0,0,1},{1,0}},
        {{h, h,0},{0,0,1},{1,1}}, {{-h, h,0},{0,0,1},{0,1}},
    };
    m.indices = { 0, 1, 2, 0, 2, 3 };
    for (const auto& v : m.verts) m.bounds.expand_point(v.pos);
    return m;
}

} // namespace meshgen
} // namespace mtt
