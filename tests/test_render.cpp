// Matted Engine - tests: mesh generators, MeshLibrary LODs, RenderQueue
// (culling, LOD selection, batching, matrix bake).
//
// NOTE on batches: FramePacket batch ORDER across (mesh,material) groups is
// unordered_map-dependent (bucket ids), so these tests assert set-equality
// and contiguity, never order.
#include "vec_check.h"

#include "engine/core/jobs/job_system.h"
#include "engine/render/mesh.h"
#include "engine/render/render_queue.h"

#include <algorithm>
#include <vector>

using namespace mtt;

static const float kEps = 1e-5f;

// ------------------------------------------------------------------ meshgen
MTT_TEST(meshgen_cube_counts_bounds_winding) {
    MeshData m = meshgen::cube();
    MTT_EQ(m.verts.size(), size_t(24));
    MTT_EQ(m.indices.size(), size_t(36));
    MTT_EQ(m.tri_count(), 12u);
    MTT_NEAR(m.bounds.mn.x, -1.0f, kEps);
    MTT_NEAR(m.bounds.mx.z, 1.0f, kEps);

    // 6 faces x 4 consecutive verts sharing one face normal
    for (int f = 0; f < 6; ++f) {
        const Vertex& v0 = m.verts[f * 4 + 0];
        for (int k = 1; k < 4; ++k) {
            const Vertex& vk = m.verts[f * 4 + k];
            v3_near(vk.normal, v0.normal, kEps, "face verts share normal", __FILE__, __LINE__);
        }
        // CCW when seen from outside: cross(b-a, c-a) points along the normal
        const Vertex& a = m.verts[f * 4 + 0];
        const Vertex& b = m.verts[f * 4 + 1];
        const Vertex& c = m.verts[f * 4 + 2];
        MTT_CHECK(dot(cross(b.pos - a.pos, c.pos - a.pos), v0.normal) > 0.0f,
                  "face winding CCW from outside");
    }
    // uv pattern per face quad: (0,0) (1,0) (1,1) (0,1)
    const Vertex& q0 = m.verts[0];
    MTT_NEAR(q0.uv.x, 0.0f, kEps);
    MTT_NEAR(m.verts[2].uv.y, 1.0f, kEps);
}

MTT_TEST(meshgen_cube_lod1_counts) {
    MeshData m = meshgen::cube_lod1();
    MTT_EQ(m.verts.size(), size_t(8));
    MTT_EQ(m.indices.size(), size_t(36));
    MTT_EQ(m.tri_count(), 12u);
    MTT_NEAR(m.bounds.mx.y, 1.0f, kEps);
}

MTT_TEST(meshgen_uv_sphere_units) {
    const int segs = 12, rings = 8;
    MeshData m = meshgen::uv_sphere(segs, rings);
    MTT_EQ(m.verts.size(), size_t((segs + 1) * (rings + 1)));
    MTT_EQ(m.indices.size(), size_t(6 * segs * rings));

    for (const Vertex& v : m.verts) {
        MTT_NEAR(length(v.pos), 1.0f, 1e-4f);        // on the unit sphere
        v3_near(v.normal, v.pos, 1e-4f, "sphere normal == position", __FILE__, __LINE__);
    }
    MTT_NEAR(m.bounds.mx.x, 1.0f, 1e-4f);
}

MTT_TEST(meshgen_plane_grid_layout) {
    const int n = 2;
    const float size = 4.0f;
    MeshData m = meshgen::plane_grid(n, size, 0);
    MTT_EQ(m.verts.size(), size_t((n + 1) * (n + 1)));
    MTT_EQ(m.indices.size(), size_t(6 * n * n));
    MTT_NEAR(m.bounds.mn.x, -size * 0.5f, kEps);
    MTT_NEAR(m.bounds.mx.z, size * 0.5f, kEps);
    MTT_NEAR(m.bounds.mn.y, 0.0f, kEps);

    for (const Vertex& v : m.verts)
        v3_near(v.normal, vec3(0, 1, 0), kEps, "plane normals +Y", __FILE__, __LINE__);

    // uv_step = 1/n when repeat == 0; vertex index = z*(n+1)+x
    const Vertex& corner = m.verts[n * (n + 1) + n];
    MTT_NEAR(corner.uv.x, 1.0f, kEps);
    MTT_NEAR(corner.uv.y, 1.0f, kEps);
}

MTT_TEST(meshgen_quad_xy) {
    MeshData m = meshgen::quad_xy(4.0f);
    MTT_EQ(m.verts.size(), size_t(4));
    MTT_EQ(m.indices.size(), size_t(6));
    MTT_NEAR(m.bounds.mx.x, 2.0f, kEps);
    MTT_NEAR(m.bounds.mn.y, -2.0f, kEps);
    MTT_NEAR(m.bounds.mx.z, 0.0f, kEps);
    for (const Vertex& v : m.verts)
        v3_near(v.normal, vec3(0, 0, 1), kEps, "quad normals +Z", __FILE__, __LINE__);
}

// ------------------------------------------------------------------ MeshLibrary
MTT_TEST(mesh_library_lod_chain_and_fallback) {
    MeshLibrary lib;
    const MeshId base = lib.add(meshgen::cube(), "base");
    const MeshId lod1 = lib.add_lod(base, 1, meshgen::cube_lod1());
    const MeshId lod2 = lib.add_lod(base, 2, meshgen::quad_xy());

    MTT_EQ(lib.count(), size_t(3));
    MTT_EQ(lib.lod(base, 0), base);
    MTT_EQ(lib.lod(base, 1), lod1);
    MTT_EQ(lib.lod(base, 2), lod2);

    // unregistered slots fall back downwards, then to the base mesh
    const MeshId lone = lib.add(meshgen::uv_sphere(), "lonely");
    MTT_EQ(lib.lod(lone, 1), lone);
    MTT_EQ(lib.lod(lone, 2), lone);
    MTT_EQ(lib.lod(base, 1), lod1); // sanity after adds

    // invalid add_lod slots are no-ops (nothing added, chain untouched)
    const size_t before = lib.count();
    lib.add_lod(base, 0, meshgen::quad_xy()); // slot 0 is the base itself
    lib.add_lod(base, 3, meshgen::quad_xy()); // out of range
    lib.add_lod(base, 7, meshgen::quad_xy());
    MTT_EQ(lib.count(), before);
    MTT_EQ(lib.lod(base, 1), lod1);

    // names: base name kept, lods auto-suffixed
    MTT_CHECK(std::string(lib.name(base)) == "base", "base name");
    MTT_CHECK(std::string(lib.name(lod1)) == "base_lod1", "lod1 name suffix");

    // get() returns the data we stored
    MTT_EQ(lib.get(lod1).verts.size(), size_t(8));
    MTT_EQ(lib.get(lod2).verts.size(), size_t(4));
}

// ------------------------------------------------------------------ RenderQueue
namespace {

struct SceneBuilder {
    MeshLibrary lib;
    std::vector<MeshRef> refs;
    std::vector<RenderInstance> instances;

    MeshId add_mesh(MeshData&& md, const char* name) { return lib.add(std::move(md), name); }
    MeshRef& add_ref(MeshId id) {
        refs.push_back(MeshRef{ id, LodSettings{} });
        return refs.back();
    }
    RenderInstance& add_instance(MeshRef& ref, const vec3& pos, MaterialId mat = 0,
                                 const vec3& scale = vec3(1, 1, 1)) {
        RenderInstance ri;
        ri.entity = Entity{};
        ri.pos = pos;
        ri.rot = quat::identity();
        ri.scale = scale;
        ri.mesh_ref = &ref;
        ri.material = mat;
        instances.push_back(ri);
        return instances.back();
    }
};

Camera default_cam() {
    Camera cam;
    cam.pos = vec3(0, 0, 0);
    cam.yaw = 0; cam.pitch = 0;
    cam.fov_y = 60.0f * kDegToRad;
    cam.near_z = 0.1f;
    cam.far_z = 500.0f;
    return cam;
}

} // namespace

MTT_TEST(render_queue_culls_and_batches) {
    jobs::init(); // idempotent; queue is parallel

    SceneBuilder s;
    const MeshId cube = s.add_mesh(meshgen::cube(), "cube");
    MeshRef& ref = s.add_ref(cube);

    s.add_instance(ref, vec3(0, 0, -5));     // in front -> visible
    s.add_instance(ref, vec3(0, 0, 5));      // behind camera -> culled
    s.add_instance(ref, vec3(0, 50, -5));    // far above -> culled
    s.add_instance(ref, vec3(0, 0, -1000));  // beyond far plane -> culled
    s.add_instance(ref, vec3(100, 0, -5));   // far right -> culled

    RenderQueue q;
    q.set_mesh_library(&s.lib);
    FramePacket fp = q.build(default_cam(), 16.0f / 9.0f, std::span<const RenderInstance>(s.instances));

    MTT_EQ(fp.instances_total, 5u);
    MTT_EQ(fp.instances_culled, 4u);
    MTT_EQ(fp.instances.size(), size_t(1));
    MTT_EQ(fp.batches.size(), size_t(1));

    const DrawBatch& b = fp.batches[0];
    MTT_EQ(b.mesh, cube);
    MTT_EQ(b.material, 0u);
    MTT_EQ(b.first, 0u);
    MTT_EQ(b.count, 1u);

    // baked matrix for the survivor: translation (0,0,-5), identity rot, unit scale
    const mat4& m = fp.instances[0].model;
    MTT_NEAR(m.m[0], 1.0f, kEps);
    MTT_NEAR(m.m[12], 0.0f, kEps);
    MTT_NEAR(m.m[13], 0.0f, kEps);
    MTT_NEAR(m.m[14], -5.0f, kEps);
}

MTT_TEST(render_queue_lod_selection) {
    SceneBuilder s;
    const MeshId base = s.add_mesh(meshgen::cube(), "base");
    const MeshId l1 = s.add_lod(base, 1, meshgen::cube_lod1());
    const MeshId l2 = s.add_lod(base, 2, meshgen::quad_xy());
    MeshRef& ref = s.add_ref(base); // thresholds 35 / 90

    s.add_instance(ref, vec3(0, 0, -5));    // d = 5   -> slot 0
    s.add_instance(ref, vec3(0, 0, -50));   // d = 50  -> slot 1
    s.add_instance(ref, vec3(0, 0, -200));  // d = 200 -> slot 2 (still < far 500)

    RenderQueue q;
    q.set_mesh_library(&s.lib);
    FramePacket fp = q.build(default_cam(), 1.7777f, std::span<const RenderInstance>(s.instances));

    MTT_EQ(fp.instances_culled, 0u);
    MTT_EQ(fp.instances.size(), size_t(3));
    MTT_EQ(fp.lods_selected[0], 1u);
    MTT_EQ(fp.lods_selected[1], 1u);
    MTT_EQ(fp.lods_selected[2], 1u);

    // three distinct LOD meshes survived -> three single-instance batches,
    // exactly the set {base, lod1, lod2} (order is unspecified)
    MTT_EQ(fp.batches.size(), size_t(3));
    std::vector<MeshId> batch_meshes;
    u32 covered = 0;
    for (const DrawBatch& b : fp.batches) {
        batch_meshes.push_back(b.mesh);
        MTT_EQ(b.count, 1u);
        MTT_EQ(b.first, covered);
        covered += b.count;
    }
    std::sort(batch_meshes.begin(), batch_meshes.end());
    MTT_EQ(batch_meshes[0], base);
    MTT_EQ(batch_meshes[1], l1);
    MTT_EQ(batch_meshes[2], l2);
    MTT_EQ(covered, 3u);
}

MTT_TEST(render_queue_batching_by_mesh_and_material) {
    SceneBuilder s;
    const MeshId cube = s.add_mesh(meshgen::cube(), "cube");
    const MeshId sphere = s.add_mesh(meshgen::uv_sphere(), "sphere");
    MeshRef& rc = s.add_ref(cube);
    MeshRef& rs = s.add_ref(sphere);

    // combos + duplicates of one key; all within LOD0 distance and frustum
    s.add_instance(rc, vec3(-3, 2, -10), 0);
    s.add_instance(rc, vec3(3, 2, -10), 1);
    s.add_instance(rs, vec3(-3, -2, -10), 0);
    s.add_instance(rs, vec3(3, -2, -10), 1);
    s.add_instance(rc, vec3(0, 3, -12), 0);
    s.add_instance(rc, vec3(0, -3, -12), 0);

    RenderQueue q;
    q.set_mesh_library(&s.lib);
    FramePacket fp = q.build(default_cam(), 1.7777f, std::span<const RenderInstance>(s.instances));

    MTT_EQ(fp.instances_total, 6u);
    MTT_EQ(fp.instances_culled, 0u);

    // expected multiset of (mesh, material) -> count
    struct Key { MeshId mesh; MaterialId mat; };
    std::vector<std::pair<Key, u32>> got;
    u32 covered = 0;
    for (const DrawBatch& b : fp.batches) {
        got.push_back({ { b.mesh, b.material }, b.count });
        MTT_EQ(b.first, covered);          // batches tile the instance array
        covered += b.count;
    }
    MTT_EQ(covered, 6u);
    std::sort(got.begin(), got.end(), [](const auto& a, const auto& b) {
        if (a.first.mesh != b.first.mesh) return a.first.mesh < b.first.mesh;
        return a.first.mat < b.first.mat;
    });

    std::vector<std::pair<Key, u32>> want = {
        { { cube, 0 }, 3 }, { { cube, 1 }, 1 }, { { sphere, 0 }, 1 }, { { sphere, 1 }, 1 },
    };
    MTT_EQ(got.size(), want.size());
    for (size_t i = 0; i < got.size() && i < want.size(); ++i) {
        MTT_EQ(got[i].first.mesh, want[i].first.mesh);
        MTT_EQ(got[i].first.mat, want[i].first.mat);
        MTT_EQ(got[i].second, want[i].second);
    }
}

MTT_TEST(render_queue_empty_and_scaled) {
    SceneBuilder s;
    const MeshId cube = s.add_mesh(meshgen::cube(), "cube");
    MeshRef& ref = s.add_ref(cube);

    RenderQueue q;
    q.set_mesh_library(&s.lib);

    // empty scene
    FramePacket fp0 = q.build(default_cam(), 1.7777f, std::span<const RenderInstance>(s.instances));
    MTT_EQ(fp0.instances_total, 0u);
    MTT_EQ(fp0.instances_culled, 0u);
    MTT_EQ(fp0.batches.size(), size_t(0));

    // conservative AABB under scale: big box straddling the right plane is
    // visible; a tiny one far out is culled
    s.add_instance(ref, vec3(8, 0, -5), 0, vec3(10, 10, 10));   // x range [3,13] straddles
    s.add_instance(ref, vec3(100, 0, -5), 0, vec3(0.1f, 0.1f, 0.1f));
    FramePacket fp = q.build(default_cam(), 1.7777f, std::span<const RenderInstance>(s.instances));
    MTT_EQ(fp.instances_total, 2u);
    MTT_EQ(fp.instances_culled, 1u);
    MTT_EQ(fp.instances.size(), size_t(1));

    // scale baked into the model matrix: scale 10 in m[0]
    MTT_NEAR(fp.instances[0].model.m[0], 10.0f, kEps);
}
