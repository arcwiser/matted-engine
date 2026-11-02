// Matted Engine - D3D12 backend (TASK-1).
//
// Implements IRenderBackend on the GPU. The header is D3D-free on purpose
// (all API handles live in the pimpl, see d3d12_backend.cpp); consumers can
// include it without pulling in windows.h/d3d12.h.
//
// Milestone 1 (this file): device/queue/fence/offscreen-RT init, frame
// begin/end plumbing, batch accounting. Mesh upload + real rasterization
// land in milestone 2; present/readback in milestone 3.
//
// AGENTS.md ownership: AgentB. Coordinate before editing.
#pragma once
#include "engine/render/backend.h"

#include <memory>

namespace mtt {

class D3D12Backend final : public IRenderBackend {
public:
    // Same construction convention as SoftwareBackend; both libraries must
    // outlive the backend. Mesh upload reads meshes_; materials feed the
    // constant pipeline (milestone 2).
    D3D12Backend(const MeshLibrary* meshes, const MaterialLibrary* materials);
    ~D3D12Backend() override;

    D3D12Backend(const D3D12Backend&) = delete;
    D3D12Backend& operator=(const D3D12Backend&) = delete;

    void begin_frame(const Camera& cam, float aspect, u32 width, u32 height) override;
    void draw_batch(MeshId mesh, MaterialId material,
                    std::span<const BakedInstance> instances) override;
    void end_frame() override;

    // GPU backend: per interface contract this returns nullptr for now
    // (readback path is milestone 3).
    const u32* pixels(u32& out_w, u32& out_h) const override {
        out_w = width_; out_h = height_;
        return nullptr;
    }
    BackendStats stats() const override { return stats_; }

    // false => device init failed (e.g. no D3D12 on this machine); every call
    // is then a safe no-op so headless runs still complete.
    bool valid() const { return valid_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    const MeshLibrary* meshes_;
    const MaterialLibrary* materials_;

    u32 width_ = 0, height_ = 0;
    BackendStats stats_;
    bool valid_ = false;
};

} // namespace mtt
