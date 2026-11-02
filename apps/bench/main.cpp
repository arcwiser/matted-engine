// Matted Engine - headless benchmarks.
//   bench render  [instances] [frames] [width height]
//   bench physics [crates]    [frames]
//   bench all
// Prints avg/p50/p99 frame (or step) times plus engine stats. Software backend
// dumps the final frame to bench_render.bmp.
#include "engine/core/jobs/job_system.h"
#include "engine/core/log.h"
#include "engine/core/math/math.h"
#include "engine/core/profiler.h"
#include "engine/physics/physics.h"
#include "engine/render/render_queue.h"
#include "engine/render/software/software_backend.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace mtt;
using namespace mtt::physics;

namespace {

inline double to_ms(i64 ns) { return (double)ns / 1e6; }

void report(const char* name, std::vector<double>& ms) {
    if (ms.empty()) return;
    std::sort(ms.begin(), ms.end());
    double sum = 0;
    for (double v : ms) sum += v;
    const double avg = sum / (double)ms.size();
    const double p50 = ms[ms.size() * 50 / 100];
    const double p99 = ms[ms.size() * 99 / 100];
    log::info("%-8s  avg %7.3f ms   p50 %7.3f   p99 %7.3f   min %7.3f   max %7.3f  (n=%d)",
              name, avg, p50, p99, ms.front(), ms.back(), (int)ms.size());
}

// machine-parseable summary line for tools/perf/bench.py: "BENCH <suite> k=v ..."
void emit_bench(const char* suite, std::vector<double>& ms) {
    if (ms.empty()) return;
    std::sort(ms.begin(), ms.end());
    double sum = 0;
    for (double v : ms) sum += v;
    std::printf("BENCH %s p99_ms=%.3f mean_ms=%.3f min_ms=%.3f n=%d\n",
                suite, ms[ms.size() * 99 / 100], sum / (double)ms.size(),
                ms.front(), (int)ms.size());
}

// ---------------------------------------------------------------- render bench
int bench_render(int instances, int frames, int width, int height) {
    log::info("bench render: %d instances, %d frames, %dx%d", instances, frames, width, height);

    MeshLibrary meshes;
    MaterialLibrary materials;
    RenderQueue queue;
    queue.set_mesh_library(&meshes);

    const MeshId cube = meshes.add(meshgen::cube(), "cube");
    meshes.add_lod(cube, 1, meshgen::cube_lod1());
    const MaterialId mat = materials.add(Material{ {0.72f, 0.62f, 0.42f}, 2.0f, 0.7f }, "m");

    std::vector<MeshRef> refs{ MeshRef{ cube, {50.0f, 120.0f} } };

    const int side = (int)std::sqrt((double)instances);
    std::vector<RenderInstance> insts((size_t)side * side);
    for (int z = 0; z < side; ++z)
        for (int x = 0; x < side; ++x) {
            RenderInstance& ri = insts[(size_t)z * side + x];
            ri.mesh_ref = &refs[0];
            ri.material = mat;
            ri.pos = vec3((float)x * 2.2f, (float)((x * 7 + z * 13) % 5) * 0.5f, (float)z * 2.2f);
            ri.rot = quat::identity();
            ri.scale = vec3(1.0f);
        }
    log::info("scene: %d instances on a %d x %d grid", side * side, side, side);

    SoftwareBackend backend(&meshes, &materials);
    Camera cam;
    cam.pos = vec3(side * 1.1f, 14.0f, side * 1.1f + 20.0f);
    cam.yaw = (float)kPi * 0.75f;
    cam.pitch = -0.25f;

    std::vector<double> queue_ms, draw_ms;
    for (int f = 0; f < frames; ++f) {
        // slow orbit for stable, representative culling work
        cam.yaw += 0.0015f;
        const auto t0 = std::chrono::steady_clock::now();
        FramePacket packet = queue.build(cam, (float)width / (float)height, insts);
        const auto t1 = std::chrono::steady_clock::now();
        backend.begin_frame(cam, (float)width / (float)height, (u32)width, (u32)height);
        for (const DrawBatch& b : packet.batches) {
            std::span<const BakedInstance> slice(packet.instances.data() + b.first, b.count);
            backend.draw_batch(b.mesh, b.material, slice);
        }
        backend.end_frame();
        const auto t2 = std::chrono::steady_clock::now();

        if (f >= 10) { // warmup excluded
            queue_ms.push_back(to_ms((i64)(t1 - t0).count()));
            draw_ms.push_back(to_ms((i64)(t2 - t1).count()));
        }
    }
    report("queue", queue_ms);
    report("draw", draw_ms);

    // combined frame time (queue + draw) for the perf harness
    std::vector<double> total_ms(queue_ms.size());
    for (size_t i = 0; i < queue_ms.size(); ++i) total_ms[i] = queue_ms[i] + draw_ms[i];
    emit_bench("render_frame", total_ms);

    const BackendStats st = backend.stats();
    u32 fw = 0, fh = 0;
    backend.pixels(fw, fh);
    (void)fw; (void)fh;
    backend.save_bmp("bench_render.bmp");
    log::info("final frame: instances_drawn %u  tris_rasterized %lluk  -> bench_render.bmp",
              st.instances_drawn, (unsigned long long)(st.tris_rasterized / 1000));
    if (!total_ms.empty()) {
        const double p99 = total_ms[total_ms.size() * 99 / 100];
        double sum = 0; for (double v : total_ms) sum += v;
        const double mean = sum / (double)total_ms.size();
        std::printf("BENCH render p99_ms=%.3f mean_ms=%.3f fps=%.1f instances=%d batches=%u tris=%llu\n",
                    p99, mean, 1000.0 / mean, side * side, st.batches_drawn,
                    (unsigned long long)st.tris_rasterized);
    }
    return 0;
}

// ---------------------------------------------------------------- physics bench
int bench_physics(int crates, int frames) {
    log::info("bench physics: %d crates, %d frames", crates, frames);
    World world;
    world.config().substeps = 2;
    world.config().velocity_iterations = 6;

    Body ground;
    ground.shape = Shape::plane(vec3(0, 1, 0), 0.0f);
    ground.flags = kBodyStatic;
    world.add_body(ground);

    // stockpile tower: stack layers of 10x10 until `crates` reached
    const float s = 1.0f;
    int placed = 0;
    for (int y = 0; placed < crates; ++y)
        for (int z = 0; z < 10 && placed < crates; ++z)
            for (int x = 0; x < 10 && placed < crates; ++x) {
                Body b;
                b.shape = Shape::box(vec3(s * 0.5f));
                b.pos = vec3((x - 4.5f) * (s + 0.02f),
                             0.5f * s + y * (s + 0.02f),
                             (z - 4.5f) * (s + 0.02f));
                b.inv_mass = 1.0f / 12.0f;
                b.flags = kBodyCanSleep | kBodyFracturable;
                b.mat = { 0.65f, 0.08f };
                world.add_body(b);
                ++placed;
            }
    log::info("spawned %d crates", placed);

    // wrecking ball arrives mid-run
    Body ball;
    ball.shape = Shape::sphere(0.9f);
    ball.pos = vec3(-30.0f, 6.0f, 0);
    ball.lin_vel = vec3(55.0f, 0, 0);
    ball.inv_mass = 1.0f / 120.0f;
    ball.mat = { 0.4f, 0.4f };
    world.add_body(ball);

    std::vector<double> step_ms;
    u32 peak_contacts = 0, total_fractures = 0;
    for (int f = 0; f < frames; ++f) {
        const auto t0 = std::chrono::steady_clock::now();
        world.step(1.0f / 60.0f);
        const auto t1 = std::chrono::steady_clock::now();
        if (f >= 10) step_ms.push_back(to_ms((i64)(t1 - t0).count()));
        peak_contacts = maxu(peak_contacts, world.stats().contacts);
        total_fractures = world.stats().fractures;
    }
    report("step", step_ms);
    const WorldStats st = world.stats();
    log::info("final: bodies %u  awake %u  contacts %u (peak %u)  fractures %u",
              st.bodies, st.awake, st.contacts, peak_contacts, total_fractures);
    if (!step_ms.empty()) {
        const double p99 = step_ms[step_ms.size() * 99 / 100];
        double sum = 0; for (double v : step_ms) sum += v;
        std::printf("BENCH physics p99_ms=%.3f mean_ms=%.3f bodies=%u contacts_peak=%u fractures=%u steps=%d\n",
                    p99, sum / (double)step_ms.size(), st.bodies, peak_contacts,
                    total_fractures, frames);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    jobs::init();
    const char* mode = argc > 1 ? argv[1] : "all";
    if (std::strcmp(mode, "render") == 0) {
        bench_render(argc > 2 ? std::atoi(argv[2]) : 20000,
                     argc > 3 ? std::atoi(argv[3]) : 200,
                     argc > 4 ? std::atoi(argv[4]) : 1280,
                     argc > 5 ? std::atoi(argv[5]) : 720);
    } else if (std::strcmp(mode, "physics") == 0) {
        bench_physics(argc > 2 ? std::atoi(argv[2]) : 1000,
                      argc > 3 ? std::atoi(argv[3]) : 300);
    } else { // all
        bench_render(20000, 200, 1280, 720);
        bench_physics(1000, 300);
    }
    prof::report_top(10);
    jobs::shutdown();
    return 0;
}
