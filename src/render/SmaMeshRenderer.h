#pragma once
#include "../di/api/IDeviceLostListener.h"
#include "render/api/IMeshRenderer.h"
#include "SmaSkinner.h"
#include <bgfx/bgfx.h>
#include <memory>
#include <vector>

namespace Caesura {

class BgfxShaderManager;

// ===========================================================================
//  SmaMeshRenderer — real bgfx implementation of IMeshRenderer (SMA S2/S5).
//
//  S2: CPU soft-skinning (SmaSkinner) + transient-vertex-buffer draw reusing
//  the existing pos+uv layout and the embedded texture program.
//
//  S5: GPU compute skinning — per-mesh immutable input, per-draw output and
//  immutable pose/transform snapshot, driven by a compute shader (D3D11 DXBC embedded;
//  GL 430 GLSL embedded). SkinMode::Auto picks the GPU path when the backend
//  reports BGFX_CAPS_COMPUTE (D3D11/GL); Metal/SPIR-V/Noop fall back to the
//  CPU skinner (see docs/design/skeletal-mesh-animation.md §3.1/S5).
//
//  GPU-free environments (headless tests, CI): init() is skipped and every
//  operation is a safe no-op (deferred-gpu pattern) — createMesh returns an
//  invalid handle, meshCount() still tracks bookkeeping when initialized.
// ===========================================================================

class SmaMeshRenderer final : public IMeshRenderer, public IDeviceLostListener {
public:
    SmaMeshRenderer(); // defined out-of-line (owns BgfxShaderManager)
    ~SmaMeshRenderer() override;

    // Lazy init once bgfx is up (called from createMesh/drawMesh paths).
    void init();

    bool isInitialized() const override { return m_initialized; }

    void setSkinMode(SkinMode mode) override;
    SkinMode skinMode() const override { return m_skinMode; }

    // True when the S5 compute skin pipeline is usable on this backend
    // (program built; per-dispatch allocation is checked at draw). Lets tests and
    // drivers probe capability instead of silently falling back to CPU.
    bool gpuSkinAvailable() const {
        return m_initialized
            && bgfx::isValid(m_skinProgram);
    }

    MeshHandle createMesh(const SMAMesh& mesh) override;
    void destroyMesh(MeshHandle handle) override;

    void updateMesh(MeshHandle handle,
                    const std::vector<BonePose>& poses) override;

    void drawMesh(uint16_t targetView, MeshHandle handle,
                  uint32_t dstTexId, float x, float y,
                  float scale, float opacity) override;

    size_t meshCount() const override { return m_meshes.size(); }

    // -- IDeviceLostListener ----------------------------------------------
    void onDeviceLost() override;
    void onDeviceRestored() override;

private:
    struct MeshEntry {
        MeshHandle handle;
        SMAMesh mesh; // CPU copy (skinning input)
        bgfx::IndexBufferHandle ib = BGFX_INVALID_HANDLE;
        std::vector<SmaSkinnedVertex> skinned;
        // S5 GPU skinning resources (valid when gpuSkinReady).
        // gpuIn is immutable: mesh topology and weights are uploaded once.
        bgfx::VertexBufferHandle gpuIn = BGFX_INVALID_HANDLE;
        // Poses stored by updateMesh; packed + dispatched at draw time
        // (same-view ordering: the bone upload + dispatch must precede
        // the draw submit that consumes the output buffer).
        std::vector<BonePose> pendingPoses;
        bool gpuSkinReady = false;
        bool gpuReady = false;
    };

    MeshEntry* find(MeshHandle handle);
    void uploadMeshGpuResources(MeshEntry& entry);
    void releaseMeshGpuResources(MeshEntry& entry);
    void releaseGpuResources();

    // Effective mode for a given mesh: Auto resolves against the backend
    // caps (compute supported + D3D11/GL renderer).
    bool useGpuSkin(const MeshEntry& entry) const;
    struct GpuDrawPacket {
        bgfx::VertexBufferHandle snapshot = BGFX_INVALID_HANDLE;
        bgfx::DynamicVertexBufferHandle output = BGFX_INVALID_HANDLE;
        GpuDrawPacket() = default;
        GpuDrawPacket(const GpuDrawPacket&) = delete;
        GpuDrawPacket& operator=(const GpuDrawPacket&) = delete;
        ~GpuDrawPacket();
    };
    void skinOnGpu(MeshEntry& entry, const std::vector<BonePose>& poses,
                   uint16_t targetView, float x, float y, float scale,
                   float viewW, float viewH, GpuDrawPacket& packet);
    void skinOnCpu(MeshEntry& entry, const std::vector<BonePose>& poses);

    bgfx::VertexLayout m_layout;      // output: pos + uv
    bgfx::VertexLayout m_skinLayout;  // input: pos + uv + bones + weights
    bgfx::VertexLayout m_boneLayout;  // immutable per-dispatch float4 snapshot
    bgfx::ProgramHandle m_skinProgram = BGFX_INVALID_HANDLE;
    std::unique_ptr<BgfxShaderManager> m_shaders;
    std::vector<MeshEntry> m_meshes;
    uint32_t m_nextId = 1;
    SkinMode m_skinMode = SkinMode::Auto;
    mutable bool m_skinWarningShown = false;  // one-time Gpu-force fallback note
    bool m_initialized = false;
    bool m_listenerRegistered = false;
};

} // namespace Caesura
