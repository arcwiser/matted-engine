// Matted Engine - interactive demo: massive stockpile + destruction.
// Controls: WASD move, RMB look, LMB shoot projectile, F fracture strike,
//           R reset scene, P profile dump, ESC quit.
#include "engine/core/jobs/job_system.h"
#include "engine/core/log.h"
#include "engine/core/math/math.h"
#include "engine/core/profiler.h"
#include "engine/physics/physics.h"
#include "engine/render/render_queue.h"
#include "engine/render/software/software_backend.h"
#include "engine/render/window.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace mtt;
using namespace mtt::physics;

namespace {

struct Links {
    std::vector<u32> inst_of_body;                 // BodyId -> instance slot
    std::unordered_map<u32, u32> body_of_inst;     // instance slot -> BodyId
    std::vector<u32> free_insts;
    std::vector<RenderInstance>* instances = nullptr;

    void bind(BodyId b, u32 inst) {
        if (inst_of_body.size() <= b) inst_of_body.resize((size_t)b + 1, kInvalid32);
        inst_of_body[b] = inst;
        body_of_inst[inst] = b;
    }
    void unbind(BodyId b) {
        if (b >= inst_of_body.size() || inst_of_body[b] == kInvalid32) return;
        const u32 inst = inst_of_body[b];
        inst_of_body[b] = kInvalid32;
        body_of_inst.erase(inst);
        free_insts.push_back(inst);
        (*instances)[inst].mesh_ref = nullptr; // hidden
    }
};

struct Demo {
    World world;
    MeshLibrary meshes;
    MaterialLibrary materials;
    RenderQueue queue;
    std::unique_ptr<SoftwareBackend> backend;

    std::vector<MeshRef> refs;          // stable storage
    std::vector<RenderInstance> instances;
    Links links;

    MeshId cube_id, cube_lod_id, sphere_id, ground_id;
    MaterialId mat_crate, mat_brick, mat_stone, mat_metal, mat_ground;

    u32 frame = 0;
    float fps_ema = 60.0f;

    void setup_meshes() {
        cube_id = meshes.add(meshgen::cube(), "cube");
        meshes.add_lod(cube_id, 1, meshgen::cube_lod1());
        sphere_id = meshes.add(meshgen::uv_sphere(14, 9), "sphere");
        ground_id = meshes.add(meshgen::plane_grid(48, 160.0f, 40), "ground");

        mat_crate = materials.add(Material{ {0.82f, 0.62f, 0.28f}, 2.0f, 0.72f }, "crate");
        mat_brick = materials.add(Material{ {0.70f, 0.32f, 0.24f}, 3.0f, 0.68f }, "brick");
        mat_stone = materials.add(Material{ {0.58f, 0.60f, 0.62f}, 4.0f, 0.70f }, "stone");
        mat_metal = materials.add(Material{ {0.75f, 0.78f, 0.85f}, 0.0f, 1.0f }, "metal");
        mat_ground = materials.add(Material{ {0.32f, 0.38f, 0.30f}, 20.0f, 0.80f }, "ground");

        refs.resize(5);
        refs[0] = { cube_id, {60.0f, 140.0f} };   // crate
        refs[1] = { cube_id, {40.0f, 100.0f} };   // brick
        refs[2] = { sphere_id, {1000.0f, 1e9f} }; // sphere (no LOD)
        refs[3] = { cube_id, {60.0f, 140.0f} };   // pillar
        refs[4] = { ground_id, {1e9f, 1e9f} };    // ground (no LOD)
    }

    u32 add_render(BodyId b, u32 ref_idx, MaterialId mat) {
        u32 inst;
        if (!links.free_insts.empty()) { inst = links.free_insts.back(); links.free_insts.pop_back(); }
        else { inst = (u32)instances.size(); instances.emplace_back(); }
        RenderInstance& ri = instances[inst];
        ri.mesh_ref = &refs[ref_idx];
        ri.material = mat;
        links.bind(b, inst);
        return inst;
    }

    BodyId spawn_body(const Body& b, u32 ref_idx, MaterialId mat) {
        const BodyId id = world.add_body(b);
        add_render(id, ref_idx, mat);
        return id;
    }

    void build_scene() {
        // ground
        Body ground;
        ground.shape = Shape::plane(vec3(0, 1, 0), 0.0f);
        ground.flags = kBodyStatic;
        spawn_body(ground, 4, mat_ground);

        // ---- the stockpile: 10 x 5 x 10 wall of crates
        const float s = 1.0f;
        for (int z = 0; z < 10; ++z)
            for (int y = 0; y < 5; ++y)
                for (int x = 0; x < 10; ++x) {
                    Body b;
                    b.shape = Shape::box(vec3(s * 0.5f));
                    b.pos = vec3((x - 4.5f) * (s + 0.02f),
                                 0.5f * s + y * (s + 0.02f),
                                 (z - 4.5f) * (s + 0.02f) - 18.0f);
                    b.inv_mass = 1.0f / 12.0f;
                    b.flags = kBodyCanSleep | kBodyFracturable;
                    b.mat = { 0.65f, 0.08f };
                    spawn_body(b, 0, mat_crate);
                }

        // ---- brick walls (fracturable, small)
        for (int w = 0; w < 2; ++w) {
            const float ox = w == 0 ? -14.0f : 14.0f;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 16; ++x) {
                    Body b;
                    b.shape = Shape::box(vec3(0.30f, 0.15f, 0.15f));
                    b.pos = vec3(ox + (x - 7.5f) * 0.62f + (y % 2) * 0.31f, 0.15f + y * 0.31f, -4.0f);
                    b.inv_mass = 1.0f / 3.0f;
                    b.flags = kBodyCanSleep | kBodyFracturable;
                    b.mat = { 0.55f, 0.06f };
                    spawn_body(b, 1, mat_brick);
                }
        }

        // ---- stone pillars (unfracturable) + metal spheres
        for (int i = 0; i < 4; ++i) {
            Body p;
            p.shape = Shape::box(vec3(0.45f, 2.4f, 0.45f));
            p.pos = vec3(-8.0f + i * 5.0f, 2.4f, 12.0f);
            p.inv_mass = 1.0f / 40.0f;
            p.flags = kBodyCanSleep;
            spawn_body(p, 3, mat_stone);

            Body sph;
            sph.shape = Shape::sphere(0.55f);
            sph.pos = p.pos + vec3(0, 3.4f, 0);
            sph.inv_mass = 1.0f / 8.0f;
            sph.flags = kBodyCanSleep;
            sph.mat = { 0.4f, 0.5f };
            spawn_body(sph, 2, mat_metal);
        }
    }

    void reset_scene() {
        std::vector<BodyId> to_kill;
        for (const auto& [inst, body] : links.body_of_inst)
            if (body != 0) to_kill.push_back(body); // keep ground (body 0)
        for (BodyId id : to_kill) {
            links.unbind(id);
            world.remove_body(id);
        }
        instances.clear();
        links = Links{};
        links.instances = &this->instances;
        build_scene();
    }

    void sync_transforms() {
        for (const auto& [inst, body] : links.body_of_inst) {
            const Body& b = world.body(body);
            RenderInstance& ri = instances[inst];
            ri.pos = b.pos;
            ri.rot = b.rot;
            ri.scale = vec3(1); // mesh sizes baked into shape dims
        }
    }

    void fire_projectile(const Camera& cam) {
        Body b;
        b.shape = Shape::sphere(0.28f);
        b.pos = cam.pos + cam.forward() * 1.2f;
        b.lin_vel = cam.forward() * 85.0f;
        b.inv_mass = 1.0f / 25.0f;
        b.flags = kBodyCanSleep;
        b.mat = { 0.4f, 0.35f };
        spawn_body(b, 2, mat_metal);
    }

    void fracture_strike(const Camera& cam) {
        const RayHit hit = world.raycast(cam.pos, cam.forward(), 220.0f);
        if (hit.body == kInvalidBody) return;
        world.apply_impulse(hit.body, cam.forward() * 400.0f, hit.point);
        world.fracture(hit.body, hit.point, 900.0f);
    }
};

} // namespace

int main() {
    jobs::init();
    log::info("Matted Engine demo starting");

    Demo demo;
    demo.setup_meshes();
    demo.queue.set_mesh_library(&demo.meshes);
    demo.backend = std::make_unique<SoftwareBackend>(&demo.meshes, &demo.materials);
    demo.links.instances = &demo.instances;
    demo.build_scene();

    Window window;
    if (!window.create("Matted Engine - stockpile destruction demo", 1280, 720)) {
        log::error("failed to create window");
        return 1;
    }

    Camera cam;
    cam.pos = vec3(0, 6, 24);
    cam.yaw = 0.0f;
    cam.pitch = -0.15f;

    const float dt_fixed = 1.0f / 60.0f;
    double accum = 0.0;
    auto last = std::chrono::steady_clock::now();
    char hud[256];

    while (window.pump()) {
        MTT_PROFILE_ZONE("frame");
        prof::frame_begin();
        const auto now = std::chrono::steady_clock::now();
        const double frame_dt = std::chrono::duration<double>(now - last).count();
        last = now;
        if (frame_dt > 0.0) demo.fps_ema = demo.fps_ema * 0.9 + (1.0 / frame_dt) * 0.1;
        accum += frame_dt;

        // ---- input / camera
        i32 mdx = 0, mdy = 0;
        window.mouse_delta(mdx, mdy);
        cam.yaw   -= mdx * 0.0032f;
        cam.pitch -= mdy * 0.0032f;
        cam.pitch = clampf(cam.pitch, -1.45f, 1.45f);

        const float speed = window.key_down(0x10) ? 22.0f : 10.0f; // shift = fast
        const vec3 fwd = cam.forward();
        const vec3 right = cam.right();
        if (window.key_down('W')) cam.pos += fwd * speed * (float)frame_dt;
        if (window.key_down('S')) cam.pos -= fwd * speed * (float)frame_dt;
        if (window.key_down('A')) cam.pos -= right * speed * (float)frame_dt;
        if (window.key_down('D')) cam.pos += right * speed * (float)frame_dt;
        if (window.key_down('E')) cam.pos += vec3(0, 1, 0) * speed * (float)frame_dt;
        if (window.key_down('Q')) cam.pos -= vec3(0, 1, 0) * speed * (float)frame_dt;

        if (window.mouse_clicked(Window::kMouseLeft)) demo.fire_projectile(cam);
        if (window.key_pressed('F')) demo.fracture_strike(cam);
        if (window.key_pressed('R')) demo.reset_scene();
        if (window.key_pressed('P')) prof::report_top(10);

        // ---- physics (fixed timestep, capped)
        u32 steps = 0;
        while (accum >= dt_fixed && steps < 3) {
            demo.world.step(dt_fixed);
            accum -= dt_fixed;
            ++steps;
        }
        if (steps == 3) accum = 0.0; // shed time instead of spiraling
        demo.sync_transforms();

        // ---- render
        u32 w = window.width(), h = window.height();
        const FramePacket packet = demo.queue.build(cam, (float)w / (float)h, demo.instances);
        demo.backend->begin_frame(cam, (float)w / (float)h, w, h);
        for (const DrawBatch& batch : packet.batches) {
            std::span<const BakedInstance> slice(packet.instances.data() + batch.first, batch.count);
            demo.backend->draw_batch(batch.mesh, batch.material, slice);
        }
        demo.backend->end_frame();

        const WorldStats st = demo.world.stats();
        const BackendStats bs = demo.backend->stats();
        std::snprintf(hud, sizeof(hud),
                      "fps %5.1f  |  bodies %u (awake %u)  contacts %u  fractures %u  |  "
                      "culled %u/%u  batches %zu  tris %lluk  |  LMB shoot  F fracture  R reset  P prof",
                      demo.fps_ema, st.bodies, st.awake, st.contacts, st.fractures,
                      packet.instances_culled, packet.instances_total,
                      packet.batches.size(),
                      (unsigned long long)(bs.tris_rasterized / 1000));
        window.present(demo.backend->pixels(w, h), w, h, hud);

        prof::frame_end();
        ++demo.frame;
    }

    log::info("demo exit after %u frames", demo.frame);
    jobs::shutdown();
    return 0;
}
