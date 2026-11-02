// Matted Engine - tile-based multithreaded software rasterizer.
//
// Pipeline per draw_batch:
//   A) jobified transform: verts -> clip -> screen space (frustum/near culled),
//      producing per-instance screen bboxes
//   B) jobified binning: instance indices -> per-thread per-tile lists
//   C) jobified raster: each tile rasterizes the triangles of its binned
//      instances (edge functions, 1/w-correct UVs, Lambert lighting, z-test)
//
// No external dependencies; targets /O2 with auto-vectorization. This is the
// reference backend for TASK-1 (D3D12) per AGENTS.md.
#pragma once
#include "engine/render/backend.h"

namespace mtt {

class SoftwareBackend final : public IRenderBackend {
public:
    SoftwareBackend(const MeshLibrary* meshes, const MaterialLibrary* materials)
        : meshes_(meshes), materials_(materials) {}

    void begin_frame(const Camera& cam, float aspect, u32 width, u32 height) override;
    void draw_batch(MeshId mesh, MaterialId material,
                    std::span<const BakedInstance> instances) override;
    void end_frame() override {}

    const u32* pixels(u32& out_w, u32& out_h) const override {
        out_w = width_; out_h = height_;
        return color_.data();
    }
    BackendStats stats() const override { return stats_; }

    // headless dump (bench app)
    bool save_bmp(const char* path) const;

    static constexpr u32 kTile = 32;

private:
    struct VertScratch {
        float x, y, z;      // screen xy, depth in [0,1]
        float invw;         // 1/clip.w (<=0 means behind near plane)
        float uw, vw;       // uv * invw (perspective-correct)
        float nx, ny, nz;   // world-space normal
    };
    struct InstScratch {
        float bb[4];        // screen bbox (xmin,ymin,xmax,ymax), clamped
        float cx, cy, cz;   // instance center (world) for lighting orientation
        u32   base;         // first VertScratch of this instance
    };

    const MeshLibrary* meshes_;
    const MaterialLibrary* materials_;

    u32 width_ = 0, height_ = 0;
    u32 tiles_x_ = 0, tiles_y_ = 0;
    std::vector<u32>  color_;
    std::vector<float> depth_;

    // per-frame scratch (sized per batch)
    std::vector<VertScratch> verts_;
    std::vector<InstScratch> xfs_;
    // per-thread bins: [worker][tile] -> instance indices
    std::vector<std::vector<std::vector<u32>>> bins_;

    Camera cam_;
    BackendStats stats_;
};

} // namespace mtt
