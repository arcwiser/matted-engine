// Matted Engine - D3D12 backend, milestone 1+2 implementation.
//
// Headless-first: no swapchain; renders into an offscreen BGRA target + depth
// buffer kept on the GPU. pixels() returns nullptr per the IRenderBackend
// contract until the readback path lands (milestone 3).
//
// Pipeline: begin_frame opens the command list (clear RT+DS), each
// draw_batch lazily uploads its MeshData to GPU buffers and records an
// instanced DrawIndexedInstanced, end_frame closes+executes+fences.
//
// Shading intentionally mirrors SoftwareBackend exactly (parity target):
//   L        = normalize(0.45, 0.85, 0.30)
//   amb      = 0.35 + 0.10 * (n.y * 0.5 + 0.5)
//   diff     = max(dot(n, L), 0)          (two-sided: no backface cull)
//   shade    = amb + diff * 0.85
//   checker  = floor(u*checker) + floor(v*checker), odd -> albedo *= checker_dim
#include "engine/render/d3d12/d3d12_backend.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "engine/core/log.h"
#include "engine/core/profiler.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace mtt {

using Microsoft::WRL::ComPtr;

namespace {

constexpr D3D_FEATURE_LEVEL kFeatureLevel = D3D_FEATURE_LEVEL_11_0;
constexpr DXGI_FORMAT      kRtFormat     = DXGI_FORMAT_B8G8R8A8_UNORM; // matches u32 BGRA8888
constexpr DXGI_FORMAT      kDepthFormat  = DXGI_FORMAT_D32_FLOAT;
constexpr u32              kVertexStride  = 32; // pos(12) + normal(12) + uv(8)
constexpr u32              kInstanceStride = 64; // mat4

// Column-major engine mat4 memory == HLSL default column_major cbuffer
// layout, so viewproj uploads bit-exact. Instance matrices arrive through the
// input assembler (attribute order = memory order = columns); assembling them
// with float4x4(row0,row1,row2,row3) yields the TRANSPOSE, hence transpose().
constexpr const char* kVertexShader = R"HLSL(
cbuffer Frame : register(b0) {
    float4x4 viewproj;      // indices 0..3 (column-major memory, matches engine)
    float4   lightPad;      // index 4
    float4   albedoChecker; // index 5
    float4   checkerDimPad; // index 6
};

struct VSIn {
    float3 pos   : POSITION;
    float3 nrm   : NORMAL;
    float2 uv    : TEXCOORD0;
    float4 w0    : TEXCOORD1;
    float4 w1    : TEXCOORD2;
    float4 w2    : TEXCOORD3;
    float4 w3    : TEXCOORD4;
};

struct VSOut {
    float4 pos : SV_Position;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD0;
};

VSOut main(VSIn i) {
    float4x4 world = transpose(float4x4(i.w0, i.w1, i.w2, i.w3));
    VSOut o;
    float4 wp = mul(world, float4(i.pos, 1.0f));
    o.pos = mul(viewproj, wp);
    o.nrm = normalize(mul((float3x3)world, i.nrm));
    o.uv  = i.uv;
    return o;
}
)HLSL";

constexpr const char* kPixelShader = R"HLSL(
cbuffer Frame : register(b0) {
    float4x4 viewproj;
    float4   lightPad;      // xyz = light dir
    float4   albedoChecker; // rgb = albedo, w = checker repeat (0 = off)
    float4   checkerDimPad; // x = checker_dim
};

struct PSIn {
    float4 pos : SV_Position;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD0;
};

float4 main(PSIn i) : SV_Target {
    float3 n = normalize(i.nrm);
    float3 L = normalize(lightPad.xyz);
    const float amb  = 0.35f + 0.10f * (n.y * 0.5f + 0.5f);
    const float diff = max(dot(n, L), 0.0f);
    const float shade = amb + diff * 0.85f;

    float3 albedo = albedoChecker.rgb;
    if (albedoChecker.w > 0.0f) {
        const float chk = floor(i.uv.x * albedoChecker.w) + floor(i.uv.y * albedoChecker.w);
        if (((i32)chk) & 1) albedo *= checkerDimPad.x;
    }
    return float4(albedo * shade, 1.0f);
}
)HLSL";

void hresult_check(HRESULT hr, const char* what, bool& ok) {
    if (FAILED(hr)) {
        log::error("d3d12: %s failed (hr=0x%08lX)", what, (unsigned long)hr);
        ok = false;
    }
}

ComPtr<ID3DBlob> compile_shader(const char* src, const char* target) {
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr,
                            "main", target, 0, 0, &blob, &err);
    if (FAILED(hr)) {
        log::error("d3d12: shader compile failed (%s):\n%.*s", target,
                   err ? (int)err->GetBufferSize() : 7,
                   err ? (const char*)err->GetBufferPointer() : "(none)");
        return nullptr;
    }
    return blob;
}

} // namespace

struct GpuMesh {
    ComPtr<ID3D12Resource> vb, ib, staging;
    u32 index_count = 0;
};

struct D3D12Backend::Impl {
    bool ok = false;

    ComPtr<IDXGIFactory4>             factory;
    ComPtr<IDXGIAdapter1>             adapter;
    ComPtr<ID3D12Device>              device;
    ComPtr<ID3D12CommandQueue>        queue;
    ComPtr<ID3D12CommandAllocator>    allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence>               fence;
    HANDLE                            fence_event = nullptr;
    u64                               fence_value = 0;

    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12Resource>       rt;
    D3D12_CPU_DESCRIPTOR_HANDLE  rtv_handle{};
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    ComPtr<ID3D12Resource>       depth;
    D3D12_CPU_DESCRIPTOR_HANDLE  dsv_handle{};

    ComPtr<ID3D12RootSignature>  root_sig;
    ComPtr<ID3D12PipelineState>  pso;

    // per-frame instance upload buffer (persistent map, grows on demand)
    ComPtr<ID3D12Resource> inst_upload;
    BakedInstance*         inst_map = nullptr;
    u32                    inst_capacity = 0;
    u32                    inst_used = 0;
    std::vector<ComPtr<ID3D12Resource>> retired_inst; // old capacity buffers until GPU done

    // lazily-uploaded GPU meshes (keyed by MeshId)
    std::unordered_map<MeshId, GpuMesh> mesh_cache;

    u32 width = 0, height = 0;

    // frame constants staged on the CPU, uploaded as root constants
    float frame_viewproj[16] = {};
    float frame_light[4] = {}; // xyz = light dir, w = 0

    void wait_for_gpu() {
        if (!ok) return;
        const u64 target = fence_value;
        if (fence->GetCompletedValue() < target) {
            fence->SetEventOnCompletion(target, fence_event);
            WaitForSingleObject(fence_event, 10000);
        }
        retired_inst.clear(); // safe: GPU finished everything
    }

    bool create_render_target(ID3D12Device* dev, u32 w, u32 h) {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w; d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = kRtFormat;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        const D3D12_CLEAR_VALUE clear = { kRtFormat, { 0.08f, 0.09f, 0.11f, 1.0f } };
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        hresult_check(dev->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE,
            &d, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&rt)),
            "CreateCommittedResource(offscreen RT)", ok);
        if (!ok) return false;

        rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(rt.Get(), nullptr, rtv_handle);

        // depth
        D3D12_RESOURCE_DESC dd = d;
        dd.Format = kDepthFormat;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        const D3D12_CLEAR_VALUE dclear = { kDepthFormat, { 1.0f, 0 } };
        hresult_check(dev->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE,
            &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dclear, IID_PPV_ARGS(&depth)),
            "CreateCommittedResource(depth)", ok);
        if (!ok) return false;
        dsv_handle = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = kDepthFormat;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dev->CreateDepthStencilView(depth.Get(), &dv, dsv_handle);
        return true;
    }

    bool create_root_sig_and_pso(ID3D12Device* dev) {
        // one root-constants param (32 DWORDs): viewproj[16] light[4]
        // albedo+checker[4] checker_dim+pad[4] pad[4]
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.ShaderRegister = 0;
        param.Constants.RegisterSpace = 0;
        param.Constants.Num32BitValues = 32;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1;
        rsd.pParameters = &param;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
            log::error("d3d12: root signature serialization failed");
            return false;
        }
        hresult_check(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                               IID_PPV_ARGS(&root_sig)), "CreateRootSignature", ok);
        if (!ok) return false;

        ComPtr<ID3DBlob> vs = compile_shader(kVertexShader, "vs_5_0");
        ComPtr<ID3DBlob> ps = compile_shader(kPixelShader, "ps_5_0");
        if (!vs || !ps) return false;

        D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION",  0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
            { "NORMAL",    0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
            { "TEXCOORD",  0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,   0 },
            { "TEXCOORD",  1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1,  0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "TEXCOORD",  2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "TEXCOORD",  3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "TEXCOORD",  4, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.InputLayout = { layout, 7 };
        pd.pRootSignature = root_sig.Get();
        pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // software reference is two-sided
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pd.SampleMask = 0xFFFFFFFFu;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = kRtFormat;
        pd.DSVFormat = kDepthFormat;
        pd.SampleDesc.Count = 1;
        hresult_check(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)),
                      "CreateGraphicsPipelineState", ok);
        return ok;
    }

    bool ensure_instance_capacity(ID3D12Device* dev, u32 need) {
        if (inst_capacity >= need) return true;

        u32 cap = inst_capacity ? inst_capacity : 4096;
        while (cap < need) cap *= 2;

        // retire the old buffer (batches already recorded this frame reference it)
        if (inst_upload) retired_inst.push_back(inst_upload);

        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = (u64)cap * kInstanceStride;
        d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&inst_upload)))) {
            log::error("d3d12: instance buffer alloc failed (%u instances)", cap);
            ok = false;
            return false;
        }
        D3D12_RANGE read{ 0, 0 };
        if (FAILED(inst_upload->Map(0, &read, reinterpret_cast<void**>(&inst_map)))) {
            log::error("d3d12: instance buffer map failed");
            ok = false;
            return false;
        }
        inst_capacity = cap;
        return true;
    }

    bool upload_mesh(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, const MeshData& md, GpuMesh& out) {
        const size_t vbytes = md.verts.size() * kVertexStride;
        const size_t ibytes = md.indices.size() * sizeof(u32);
        if (!vbytes || !ibytes) return false;

        D3D12_HEAP_PROPERTIES upload_heap{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_HEAP_PROPERTIES default_heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        // staging (kept alive in GpuMesh for simplicity; retire later if needed)
        bd.Width = vbytes;
        hresult_check(dev->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &bd,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&out.staging)), "mesh staging", ok);
        if (!ok) return false;
        {
            void* p = nullptr;
            D3D12_RANGE read{ 0, 0 };
            if (FAILED(out.staging->Map(0, &read, &p))) { ok = false; return false; }
            std::memcpy(p, md.verts.data(), vbytes);
            out.staging->Unmap(0, nullptr);
        }
        bd.Width = ibytes;
        ComPtr<ID3D12Resource> ib_staging;
        hresult_check(dev->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &bd,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&ib_staging)), "ib staging", ok);
        if (!ok) return false;
        {
            void* p = nullptr;
            D3D12_RANGE read{ 0, 0 };
            if (FAILED(ib_staging->Map(0, &read, &p))) { ok = false; return false; }
            std::memcpy(p, md.indices.data(), ibytes);
            ib_staging->Unmap(0, nullptr);
        }

        bd.Width = vbytes;
        hresult_check(dev->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &bd,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&out.vb)), "mesh vb", ok);
        if (!ok) return false;
        bd.Width = ibytes;
        hresult_check(dev->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &bd,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&out.ib)), "mesh ib", ok);
        if (!ok) return false;

        cl->CopyBufferRegion(out.vb.Get(), 0, out.staging.Get(), 0, vbytes);
        cl->CopyBufferRegion(out.ib.Get(), 0, ib_staging.Get(), 0, ibytes);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = out.vb.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
        b.Transition.pResource = out.ib.Get();
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER;
        cl->ResourceBarrier(1, &b);

        out.index_count = (u32)md.indices.size();
        return true;
    }

    bool init() {
        ok = true;

        UINT flags = 0;
#if defined(_DEBUG)
        flags = DXGI_CREATE_FACTORY_DEBUG;
#endif
        hresult_check(CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2", ok);
        if (!ok) return false;

        // Prefer the fastest hardware adapter; fall back to WARP (software
        // D3D12) so headless machines without a GPU still run.
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> cand;
            if (factory->EnumAdapters1(i, &cand) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            cand->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(cand.Get(), kFeatureLevel, IID_PPV_ARGS(&device)))) {
                adapter = cand;
                break;
            }
        }
        if (!device) {
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> cand;
                if (factory->EnumAdapters1(i, &cand) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 desc{};
                cand->GetDesc1(&desc);
                if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
                if (SUCCEEDED(D3D12CreateDevice(cand.Get(), kFeatureLevel, IID_PPV_ARGS(&device)))) {
                    adapter = cand;
                    break;
                }
            }
        }
        if (!device) {
            log::error("d3d12: no D3D12-capable adapter (hardware or WARP) found");
            ok = false;
            return false;
        }

        DXGI_ADAPTER_DESC1 adesc{};
        adapter->GetDesc1(&adesc);
        log::info("d3d12: device on '%ls' (feature level 0x%04X)",
                  adesc.Description, (unsigned)kFeatureLevel);

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hresult_check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue", ok);
        hresult_check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                      "CreateCommandAllocator", ok);
        hresult_check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                IID_PPV_ARGS(&list)), "CreateCommandList", ok);
        hresult_check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence", ok);
        if (!ok) return false;
        list->Close(); // created in recording state; reset before first use

        fence_event = CreateEventExW(nullptr, FALSE, FALSE, EVENT_ALL_ACCESS);
        if (!fence_event) { ok = false; return false; }

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        hresult_check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)), "CreateDescriptorHeap(RTV)", ok);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hresult_check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap)), "CreateDescriptorHeap(DSV)", ok);
        if (!ok) return false;

        if (!create_root_sig_and_pso(device.Get())) return false;
        return true;
    }

    void shutdown() {
        if (ok) wait_for_gpu();
        if (inst_map && inst_upload) inst_upload->Unmap(0, nullptr);
        inst_map = nullptr;
        if (fence_event) { CloseHandle(fence_event); fence_event = nullptr; }
        mesh_cache.clear();
        retired_inst.clear();
        inst_upload.Reset();
        depth.Reset(); dsv_heap.Reset();
        rt.Reset(); rtv_heap.Reset();
        pso.Reset(); root_sig.Reset();
        list.Reset(); fence.Reset(); allocator.Reset(); queue.Reset();
        device.Reset(); adapter.Reset(); factory.Reset();
    }
};

D3D12Backend::D3D12Backend(const MeshLibrary* meshes, const MaterialLibrary* materials)
    : meshes_(meshes), materials_(materials) {
    impl_ = std::make_unique<Impl>();
    if (impl_->init()) {
        log::info("d3d12 backend ready (offscreen, milestones 1-2: raster path live)");
        valid_ = true;
    } else {
        log::error("d3d12 backend init failed - running as no-op (headless-safe)");
        valid_ = false;
    }
}

D3D12Backend::~D3D12Backend() {
    if (impl_) impl_->shutdown();
}

void D3D12Backend::begin_frame(const Camera& cam, float aspect, u32 width, u32 height) {
    MTT_PROFILE_ZONE("d3d12.begin_frame");
    if (!valid_ || width == 0 || height == 0) return;

    impl_->wait_for_gpu();
    impl_->inst_used = 0;

    if (impl_->width != width || impl_->height != height || !impl_->rt) {
        impl_->rt.Reset();
        impl_->depth.Reset();
        if (!impl_->create_render_target(impl_->device.Get(), width, height)) {
            valid_ = false;
            return;
        }
        impl_->width = width;
        impl_->height = height;
        log::info("d3d12: render target %ux%u", width, height);
    }

    if (FAILED(impl_->allocator->Reset()) || FAILED(impl_->list->Reset(impl_->allocator.Get(), impl_->pso.Get()))) {
        log::error("d3d12: command list/allocator reset failed");
        valid_ = false;
        return;
    }

    // frame constants (root constants): viewproj + light dir
    std::memcpy(&impl_->frame_viewproj[0], cam.view_proj(aspect).m, sizeof(float) * 16);
    const vec3 L = normalize(vec3(0.45f, 0.85f, 0.30f)); // same as software reference
    impl_->frame_light[0] = L.x; impl_->frame_light[1] = L.y; impl_->frame_light[2] = L.z;

    impl_->list->SetGraphicsRootSignature(impl_->root_sig.Get());
    impl_->list->SetGraphicsRoot32BitConstants(0, 16, impl_->frame_viewproj, 0);
    impl_->list->SetGraphicsRoot32BitConstants(0, 4, impl_->frame_light, 16);

    const D3D12_VIEWPORT vp{ 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
    const D3D12_RECT sc{ 0, 0, (LONG)width, (LONG)height };
    impl_->list->RSSetViewports(1, &vp);
    impl_->list->RSSetScissorRects(1, &sc);
    impl_->list->OMSetRenderTargets(1, &impl_->rtv_handle, FALSE, &impl_->dsv_handle);

    const float clear[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
    impl_->list->ClearRenderTargetView(impl_->rtv_handle, clear, 0, nullptr);
    impl_->list->ClearDepthStencilView(impl_->dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    impl_->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void D3D12Backend::draw_batch(MeshId mesh, MaterialId material,
                              std::span<const BakedInstance> instances) {
    MTT_PROFILE_ZONE("d3d12.draw_batch");
    if (!valid_ || instances.empty()) return;
    if (!meshes_ || mesh >= meshes_->count()) return;

    stats_.batches_drawn += 1;
    stats_.instances_drawn += (u32)instances.size();

    const MeshData& md = meshes_->get(mesh);
    if (md.indices.empty()) return;
    // approximation until readback/occlusion queries (milestone 3)
    stats_.pixels_shaded += (u64)impl_->width * impl_->height;

    // material constants (root constants slot 16..19)
    const Material& mat = materials_ ? materials_->get(material) : Material{};
    const float mat_consts[4] = { mat.albedo.x, mat.albedo.y, mat.albedo.z, mat.checker };
    const float mat_consts2[4] = { mat.checker_dim, 0.0f, 0.0f, 0.0f };
    impl_->list->SetGraphicsRoot32BitConstants(0, 4, mat_consts, 20);
    impl_->list->SetGraphicsRoot32BitConstants(0, 4, mat_consts2, 24);

    // lazy GPU mesh upload
    auto it = impl_->mesh_cache.find(mesh);
    if (it == impl_->mesh_cache.end()) {
        GpuMesh gm;
        if (!impl_->upload_mesh(impl_->device.Get(), impl_->list.Get(), md, gm)) {
            log::error("d3d12: mesh upload failed (MeshId %u)", mesh);
            valid_ = false;
            return;
        }
        it = impl_->mesh_cache.emplace(mesh, std::move(gm)).first;
    }
    const GpuMesh& gm = it->second;

    // instance data into the frame upload buffer
    if (!impl_->ensure_instance_capacity(impl_->device.Get(), impl_->inst_used + (u32)instances.size())) {
        valid_ = false;
        return;
    }
    const u32 base = impl_->inst_used;
    std::memcpy(impl_->inst_map + base, instances.data(), instances.size() * sizeof(BakedInstance));
    impl_->inst_used += (u32)instances.size();

    D3D12_VERTEX_BUFFER_VIEW vbs[2] = {};
    vbs[0].BufferLocation = gm.vb->GetGPUVirtualAddress();
    vbs[0].SizeInBytes = (u32)(md.verts.size() * kVertexStride);
    vbs[0].StrideInBytes = kVertexStride;
    vbs[1].BufferLocation = impl_->inst_upload->GetGPUVirtualAddress() + (u64)base * kInstanceStride;
    vbs[1].SizeInBytes = (impl_->inst_capacity - base) * kInstanceStride;
    vbs[1].StrideInBytes = kInstanceStride;
    impl_->list->IASetVertexBuffers(0, 2, vbs);

    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = gm.ib->GetGPUVirtualAddress();
    ibv.SizeInBytes = (u32)(md.indices.size() * sizeof(u32));
    ibv.Format = DXGI_FORMAT_R32_UINT;
    impl_->list->IASetIndexBuffer(&ibv);

    impl_->list->DrawIndexedInstanced(gm.index_count, (u32)instances.size(), 0, 0, 0);
    stats_.tris_rasterized += (u64)md.tri_count() * instances.size();
}

void D3D12Backend::end_frame() {
    MTT_PROFILE_ZONE("d3d12.end_frame");
    if (!valid_) return;

    if (FAILED(impl_->list->Close())) {
        log::error("d3d12: command list close failed");
        valid_ = false;
        return;
    }
    ID3D12CommandList* lists[] = { impl_->list.Get() };
    impl_->queue->ExecuteCommandLists(1, lists);
    impl_->fence_value += 1;
    impl_->queue->Signal(impl_->fence.Get(), impl_->fence_value);
}

} // namespace mtt

#if defined(_MSC_VER)
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")
#endif
