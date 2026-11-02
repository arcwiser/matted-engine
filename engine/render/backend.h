// Matted Engine - render backend interface.
//
// A backend receives a FramePacket batch-by-batch:
//   begin_frame(camera, aspect, w, h)  -> bind/resize target, clear
//   draw_batch(mesh, material, ...)    -> one (mesh,material) run of instances
//   end_frame()                        -> finalize
//   pixels()                           -> software backends expose the framebuffer
//
// CONSTRAINT: this header must stay free of any graphics-API includes so GPU
// backends (D3D12/Vulkan, see AGENTS.md TASK-1) can implement it cleanly.
#pragma once
#include <span>
#include "engine/core/mtt_types.h"
#include "engine/render/mesh.h"        // MeshId, MaterialId, Camera
#include "engine/render/render_queue.h" // BakedInstance

namespace mtt {

struct BackendStats {
    u32 batches_drawn = 0;
    u32 instances_drawn = 0;
    u64 tris_rasterized = 0;
    u64 pixels_shaded = 0;
};

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    virtual void begin_frame(const Camera& cam, float aspect, u32 width, u32 height) = 0;
    virtual void draw_batch(MeshId mesh, MaterialId material,
                            std::span<const BakedInstance> instances) = 0;
    virtual void end_frame() = 0;

    // Software backends return a pointer to packed BGRA8888 pixels
    // (u32 = 0xFF<<24 | r<<16 | g<<8 | b) with the given dimensions; GPU
    // backends may return nullptr (their present path is backend-specific).
    virtual const u32* pixels(u32& out_w, u32& out_h) const = 0;

    virtual BackendStats stats() const = 0;
};

} // namespace mtt
