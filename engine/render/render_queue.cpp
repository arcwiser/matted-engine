#include "engine/render/render_queue.h"
#include "engine/core/jobs/job_system.h"
#include "engine/core/profiler.h"
#include <algorithm>
#include <chrono>
#include <unordered_map>

namespace mtt {

FramePacket RenderQueue::build(const Camera& cam, float aspect,
                               std::span<const RenderInstance> instances) {
    MTT_PROFILE_ZONE("queue.build");
    FramePacket out;
    const u64 n = instances.size();
    out.instances_total = (u32)n;

    const mat4 vp = cam.view_proj(aspect);
    frustum fru;
    fru.build_from_viewproj(vp);

    // -------------------------------------------------- pass 1: frustum cull + LOD pick (parallel)
    struct VisItem {
        u32 idx;        // instance index
        MeshId mesh;    // LOD-selected mesh variant
        u32 slot;       // LOD slot used (for stats)
    };
    const u32 n_threads = jobs::thread_count() + 1;
    std::vector<std::vector<VisItem>> per_thread(n_threads);
    for (auto& v : per_thread) v.reserve(1024);

    jobs::parallel_for(n, 512, [&](u64 begin, u64 end) {
        std::vector<VisItem>& local = per_thread[jobs::worker_index()];
        for (u64 i = begin; i < end; ++i) {
            const RenderInstance& ri = instances[i];
            const MeshData& md = meshes_->get(ri.mesh_ref->mesh);
            const aabb world = md.bounds.transformed(ri.pos, ri.rot, ri.scale);
            if (!fru.intersects(world)) continue;

            // LOD by distance to camera (squared, compare against thresholds)
            const float d2 = dist_sq(ri.pos, cam.pos);
            u32 slot = 0;
            if (d2 > ri.mesh_ref->lod.lod2_dist * ri.mesh_ref->lod.lod2_dist) slot = 2;
            else if (d2 > ri.mesh_ref->lod.lod1_dist * ri.mesh_ref->lod.lod1_dist) slot = 1;

            local.push_back(VisItem{ (u32)i, meshes_->lod(ri.mesh_ref->mesh, slot), slot });
        }
    });

    // concat worker-local lists
    size_t vis_total = 0;
    for (const auto& v : per_thread) vis_total += v.size();
    std::vector<VisItem> vis;
    vis.reserve(vis_total);
    for (auto& v : per_thread) {
        vis.insert(vis.end(), v.begin(), v.end());
        v.clear();
        v.shrink_to_fit();
    }
    out.instances_culled = out.instances_total - (u32)vis.size();

    // -------------------------------------------------- pass 2: counting sort by (mesh, material)
    // Key = (meshId << 32) | materialId. Distinct keys are few (mesh/material count),
    // so counting sort beats comparison sorts at 100k+ instances.
    std::unordered_map<u64, u32> bucket_of;
    bucket_of.reserve(vis.size() / 4 + 8);
    for (const VisItem& v : vis) {
        const u64 key = ((u64)v.mesh << 32) | instances[v.idx].material;
        bucket_of.emplace(key, 0); // first occurrence wins the id
    }
    { // assign dense bucket ids
        u32 next = 0;
        for (auto& [key, id] : bucket_of) id = next++;
    }
    std::vector<u32> counts(bucket_of.size() + 1, 0);
    for (const VisItem& v : vis) {
        const u64 key = ((u64)v.mesh << 32) | instances[v.idx].material;
        ++counts[bucket_of[key] + 1];
    }
    for (size_t b = 1; b <= bucket_of.size(); ++b) counts[b] += counts[b - 1]; // prefix
    std::vector<VisItem> sorted(vis.size());
    {
        // scatter (stable). offsets live in counts[bucket]
        std::vector<u32> cursor(counts.begin(), counts.end() - 1);
        for (const VisItem& v : vis) {
            const u64 key = ((u64)v.mesh << 32) | instances[v.idx].material;
            sorted[cursor[bucket_of[key]]++] = v;
        }
    }

    // -------------------------------------------------- pass 3: batch runs
    const auto key_at = [&](size_t k) {
        return ((u64)sorted[k].mesh << 32) | (u64)instances[sorted[k].idx].material;
    };
    out.batches.clear();
    size_t i = 0;
    while (i < sorted.size()) {
        const u64 key0 = key_at(i);
        size_t j = i + 1;
        while (j < sorted.size() && key_at(j) == key0) ++j;
        out.batches.push_back(DrawBatch{
            sorted[i].mesh, instances[sorted[i].idx].material, (u32)i, (u32)(j - i) });
        i = j;
    }

    // -------------------------------------------------- pass 4: bake instance matrices (parallel)
    out.instances.resize(sorted.size());
    jobs::parallel_for(sorted.size(), 256, [&](u64 begin, u64 end) {
        for (u64 k = begin; k < end; ++k) {
            const RenderInstance& ri = instances[sorted[k].idx];
            out.instances[k].model = mat4::trs(ri.pos, ri.rot, ri.scale);
            out.lods_selected[sorted[k].slot]++;
        }
    });

    return out;
}

} // namespace mtt
