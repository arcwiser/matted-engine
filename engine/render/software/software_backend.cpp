#include "engine/render/software/software_backend.h"
#include "engine/core/jobs/job_system.h"
#include "engine/core/profiler.h"
#include <cstdio>
#include <cstring>

namespace mtt {

// ------------------------------------------------------------------ helpers
static inline u32 pack_rgba(u8 r, u8 g, u8 b) {
    return 0xFF000000u | (u32(r) << 16) | (u32(g) << 8) | u32(b);
}
static inline u32 tile_count(u32 span, u32 tile) { return (span + tile - 1) / tile; }

// ------------------------------------------------------------------ frame
void SoftwareBackend::begin_frame(const Camera& cam, float aspect, u32 width, u32 height) {
    MTT_PROFILE_ZONE("sw.begin_frame");
    cam_ = cam;
    if (width_ != width || height_ != height) {
        width_ = width; height_ = height;
        color_.assign(size_t(width_) * height_, 0xFF000000u);
        depth_.assign(size_t(width_) * height_, 1e30f);
        tiles_x_ = tile_count(width_, kTile);
        tiles_y_ = tile_count(height_, kTile);
    } else {
        std::fill(color_.begin(), color_.end(), 0xFF000000u);
        std::fill(depth_.begin(), depth_.end(), 1e30f);
    }
    stats_ = BackendStats{};
}

// ------------------------------------------------------------------ draw_batch
void SoftwareBackend::draw_batch(MeshId mesh_id, MaterialId material_id,
                                 std::span<const BakedInstance> instances) {
    if (instances.empty()) return;
    MTT_PROFILE_ZONE("sw.draw_batch");
    const MeshData& mesh = meshes_->get(mesh_id);
    const Material& mat = materials_->get(material_id);
    const u32 vert_count = (u32)mesh.verts.size();
    const u32 tri_count = mesh.tri_count();
    const mat4& vp = cam_.view_proj((float)width_ / (float)height_);

    const u32 n = (u32)instances.size();
    stats_.batches_drawn++;
    stats_.instances_drawn += n;

    // ---- stage A: transform + cull (parallel) --------------------------------
    MTT_PROFILE_ZONE("sw.transform");
    verts_.resize(size_t(n) * vert_count);
    xfs_.resize(n);
    const u32 n_threads = jobs::thread_count() + 1;

    jobs::parallel_for(n, 128, [&](u64 begin, u64 end) {
        for (u64 i = begin; i < end; ++i) {
            const mat4& model = instances[i].model;
            VertScratch* out = &verts_[size_t(i) * vert_count];
            float xmin = 1e30f, ymin = 1e30f, xmax = -1e30f, ymax = -1e30f;
            vec3 center(0, 0, 0);
            bool any_on_screen = false;

            for (u32 v = 0; v < vert_count; ++v) {
                const Vertex& src = mesh.verts[v];
                const vec3 world = model.transform_point(src.pos);
                const vec4 clip = vp.transform_vec4(vec4(world, 1.0f));
                VertScratch& dst = out[v];
                dst.invw = clip.w > 1e-6f ? 1.0f / clip.w : -1.0f;
                if (dst.invw <= 0.0f) { // behind near plane
                    dst.x = -1e9f; dst.y = -1e9f; dst.z = 2.0f;
                    dst.uw = dst.vw = 0.0f;
                    const vec3 nrm = normalize(model.transform_dir(src.normal));
                    dst.nx = nrm.x; dst.ny = nrm.y; dst.nz = nrm.z;
                    continue;
                }
                const float px = (clip.x * dst.invw * 0.5f + 0.5f) * (float)width_;
                const float py = (1.0f - (clip.y * dst.invw * 0.5f + 0.5f)) * (float)height_;
                const float pz = clip.z * dst.invw * 0.5f + 0.5f;
                dst.x = px; dst.y = py; dst.z = pz;
                dst.uw = src.uv.x * dst.invw;
                dst.vw = src.uv.y * dst.invw;
                const vec3 nrm = normalize(model.transform_dir(src.normal));
                dst.nx = nrm.x; dst.ny = nrm.y; dst.nz = nrm.z;

                xmin = minf(xmin, px); ymin = minf(ymin, py);
                xmax = maxf(xmax, px); ymax = maxf(ymax, py);
                center += world;
                any_on_screen = true;
            }
            (void)any_on_screen;

            InstScratch& ix = xfs_[i];
            if (xmin > xmax || ymin > ymax ||     // degenerate / fully clipped
                xmax < 0 || ymax < 0 ||
                xmin > (float)width_ || ymin > (float)height_) {
                ix.bb[0] = ix.bb[1] = 1e30f;      // empty bbox => culled
                ix.bb[2] = ix.bb[3] = -1e30f;
            } else {
                ix.bb[0] = maxf(xmin, 0.0f);
                ix.bb[1] = maxf(ymin, 0.0f);
                ix.bb[2] = minf(xmax, (float)width_ - 1e-3f);
                ix.bb[3] = minf(ymax, (float)height_ - 1e-3f);
            }
            center = center / (float)vert_count;
            ix.cx = center.x; ix.cy = center.y; ix.cz = center.z;
            ix.base = (u32)(size_t(i) * vert_count);
        }
    });

    // ---- stage B: bin instances into tiles (parallel, per-thread bins) ------
    MTT_PROFILE_ZONE("sw.bin");
    if (bins_.size() != n_threads) {
        bins_.assign(n_threads, {});
        for (auto& tb : bins_) tb.assign(tiles_x_ * tiles_y_, {});
    }
    // clear all bins ONCE (per-chunk clearing would drop earlier chunks' entries)
    for (auto& tb : bins_)
        for (auto& b : tb) b.clear();

    jobs::parallel_for(n, 128, [&](u64 begin, u64 end) {
        auto& tile_bins = bins_[jobs::worker_index()];
        for (u64 i = begin; i < end; ++i) {
            const InstScratch& ix = xfs_[i];
            if (ix.bb[0] > ix.bb[2]) continue; // culled
            const u32 tx0 = (u32)(ix.bb[0] / kTile);
            const u32 ty0 = (u32)(ix.bb[1] / kTile);
            const u32 tx1 = minu((u32)(ix.bb[2] / kTile), tiles_x_ - 1);
            const u32 ty1 = minu((u32)(ix.bb[3] / kTile), tiles_y_ - 1);
            for (u32 ty = ty0; ty <= ty1; ++ty)
                for (u32 tx = tx0; tx <= tx1; ++tx)
                    tile_bins[ty * tiles_x_ + tx].push_back((u32)i);
        }
    });

    // ---- stage C: rasterize tiles (parallel) ---------------------------------
    MTT_PROFILE_ZONE("sw.raster");
    const vec3 light_dir = normalize(vec3(0.45f, 0.85f, 0.30f));
    const vec3 cam_pos = cam_.pos;

    std::vector<u64> tris_part(n_threads, 0), px_part(n_threads, 0);

    jobs::parallel_for(u64(tiles_x_) * tiles_y_, 4, [&](u64 begin, u64 end) {
        u64 tris_local = 0, px_local = 0;
        const u32 wid = jobs::worker_index();
        for (u64 t = begin; t < end; ++t) {
            const u32 tx = (u32)(t % tiles_x_);
            const u32 ty = (u32)(t / tiles_x_);
            const i32 px0 = i32(tx * kTile);
            const i32 py0 = i32(ty * kTile);
            const i32 px1 = mini(px0 + (i32)kTile, (i32)width_);
            const i32 py1 = mini(py0 + (i32)kTile, (i32)height_);

            for (u32 w = 0; w < n_threads; ++w) {
                const std::vector<u32>& list = bins_[w][t];
                for (const u32 inst : list) {
                    const InstScratch& ix = xfs_[inst];
                    const VertScratch* vb = &verts_[ix.base];
                    const vec3 to_center = normalize(vec3(ix.cx, ix.cy, ix.cz) - cam_pos);

                    for (u32 ti = 0; ti < tri_count; ++ti) {
                        const u32 ia = mesh.indices[ti * 3 + 0];
                        const u32 ib = mesh.indices[ti * 3 + 1];
                        const u32 ic = mesh.indices[ti * 3 + 2];
                        const VertScratch& v0 = vb[ia];
                        const VertScratch& v1 = vb[ib];
                        const VertScratch& v2 = vb[ic];
                        if (v0.invw <= 0 || v1.invw <= 0 || v2.invw <= 0) continue;

                        // triangle bbox vs tile
                        const float txmin = maxf(minf(v0.x, minf(v1.x, v2.x)), (float)px0);
                        const float tymin = maxf(minf(v0.y, minf(v1.y, v2.y)), (float)py0);
                        const float txmax = minf(maxf(v0.x, maxf(v1.x, v2.x)), (float)px1 - 1e-3f);
                        const float tymax = minf(maxf(v0.y, maxf(v1.y, v2.y)), (float)py1 - 1e-3f);
                        if (txmin > txmax || tymin > tymax) continue;

                        // positive winding (no backface cull: two-sided lighting)
                        const VertScratch* a = &v0; const VertScratch* b = &v1; const VertScratch* c = &v2;
                        float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
                        if (area < 0) { std::swap(b, c); area = -area; }
                        if (area < 1e-9f) continue;
                        const float inv_area = 1.0f / area;

                        const i32 sx0 = (i32)std::floor(txmin);
                        const i32 sy0 = (i32)std::floor(tymin);
                        const i32 sx1 = (i32)std::ceil(txmax);
                        const i32 sy1 = (i32)std::ceil(tymax);

                        // edge function: E(p) = (bx-ax)*(py-ay) - (by-ay)*(px-ax)
                        const auto edge = [](const VertScratch& p0, const VertScratch& p1,
                                             float px, float py) {
                            return (p1.x - p0.x) * (py - p0.y) - (p1.y - p0.y) * (px - p0.x);
                        };
                        // incremental edge steps (dE/dx, dE/dy per edge)
                        const float e0_dx = (c->y - b->y), e0_dy = -(c->x - b->x); // E0 opposite v0: edge(b,c)
                        const float e1_dx = (a->y - c->y), e1_dy = -(a->x - c->x); // E1 opposite v1: edge(c,a)
                        const float e2_dx = (b->y - a->y), e2_dy = -(b->x - a->x); // E2 opposite v2: edge(a,b)

                        float row_e0 = edge(*b, *c, sx0 + 0.5f, sy0 + 0.5f);
                        float row_e1 = edge(*c, *a, sx0 + 0.5f, sy0 + 0.5f);
                        float row_e2 = edge(*a, *b, sx0 + 0.5f, sy0 + 0.5f);

                        for (i32 py = sy0; py <= sy1; ++py) {
                            float e0 = row_e0, e1 = row_e1, e2 = row_e2;
                            float* drow = &depth_[size_t(py) * width_];
                            u32* crow = &color_[size_t(py) * width_];
                            for (i32 px = sx0; px <= sx1; ++px) {
                                if (e0 >= 0 && e1 >= 0 && e2 >= 0) {
                                    const float l0 = e0 * inv_area;
                                    const float l1 = e1 * inv_area;
                                    const float l2 = e2 * inv_area;
                                    const float z = l0 * a->z + l1 * b->z + l2 * c->z;
                                    const size_t p = size_t(px);
                                    if (z < drow[p]) {
                                        drow[p] = z;
                                        // perspective-correct uv
                                        const float iw = l0 * a->invw + l1 * b->invw + l2 * c->invw;
                                        const float u = (l0 * a->uw + l1 * b->uw + l2 * c->uw) / iw;
                                        const float v = (l0 * a->vw + l1 * b->vw + l2 * c->vw) / iw;
                                        float nx = l0 * a->nx + l1 * b->nx + l2 * c->nx;
                                        float ny = l0 * a->ny + l1 * b->ny + l2 * c->ny;
                                        float nz = l0 * a->nz + l1 * b->nz + l2 * c->nz;
                                        // two-sided: orient normal toward viewer
                                        if (nx * to_center.x + ny * to_center.y + nz * to_center.z > 0) {
                                            nx = -nx; ny = -ny; nz = -nz;
                                        }
                                        vec3 albedo = mat.albedo;
                                        if (mat.checker > 0.0f) {
                                            const float chk = std::floor(u * mat.checker) +
                                                              std::floor(v * mat.checker);
                                            if (((i32)chk) & 1) albedo = albedo * mat.checker_dim;
                                        }
                                        const float diff = maxf(nx * light_dir.x + ny * light_dir.y +
                                                                nz * light_dir.z, 0.0f);
                                        const float amb = 0.35f + 0.10f * (ny * 0.5f + 0.5f);
                                        const float shade = amb + diff * 0.85f;
                                        crow[p] = pack_rgba(
                                            (u8)clampf(albedo.x * shade * 255.0f, 0.f, 255.f),
                                            (u8)clampf(albedo.y * shade * 255.0f, 0.f, 255.f),
                                            (u8)clampf(albedo.z * shade * 255.0f, 0.f, 255.f));
                                        px_local++;
                                    }
                                }
                                e0 += e0_dx; e1 += e1_dx; e2 += e2_dx;
                            }
                            row_e0 += e0_dy; row_e1 += e1_dy; row_e2 += e2_dy;
                        }
                        tris_local++;
                    }
                }
            }
        }
        tris_part[wid] += tris_local;
        px_part[wid] += px_local;
    });
    for (u32 w = 0; w < n_threads; ++w) {
        stats_.tris_rasterized += tris_part[w];
        stats_.pixels_shaded += px_part[w];
    }
}

// ------------------------------------------------------------------ BMP dump
bool SoftwareBackend::save_bmp(const char* path) const {
    FILE* f = nullptr;
#ifdef _WIN32
    fopen_s(&f, path, "wb");
#else
    f = std::fopen(path, "wb");
#endif
    if (!f) return false;
    const u32 row_bytes = width_ * 4;
    const u32 pad = (4 - (row_bytes % 4)) % 4;
    const u32 data_size = (row_bytes + pad) * height_;
    u8 hdr[54] = {};
    const u32 file_size = 54 + data_size;
    hdr[0] = 'B'; hdr[1] = 'M';
    std::memcpy(hdr + 2, &file_size, 4);
    const u32 data_off = 54;    std::memcpy(hdr + 10, &data_off, 4);
    const u32 dib = 40;         std::memcpy(hdr + 14, &dib, 4);
    const i32 w32 = (i32)width_, h32 = (i32)height_;
    std::memcpy(hdr + 18, &w32, 4);
    std::memcpy(hdr + 22, &h32, 4);
    const u16 planes = 1, bpp = 32;
    std::memcpy(hdr + 26, &planes, 2);
    std::memcpy(hdr + 28, &bpp, 2);
    std::memcpy(hdr + 34, &data_size, 4);
    std::fwrite(hdr, 1, 54, f);
    std::vector<u8> row(row_bytes + pad);
    for (u32 y = height_; y-- > 0;) { // BMP is bottom-up
        const u8* src = reinterpret_cast<const u8*>(&color_[size_t(y) * width_]);
        std::memcpy(row.data(), src, row_bytes);
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

} // namespace mtt
