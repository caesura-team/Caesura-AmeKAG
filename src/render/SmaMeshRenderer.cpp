#include "SmaMeshRenderer.h"
#include "BgfxShaderManager.h"
#include "EmbeddedShaders.h"
#include "SmaSkinner.h"
#include "../debug/api/DebugLog.h"   // P1-6: api header instead of concrete DebugManager.h
#include "../di/BackendRegistry.h"
#include <bgfx/bgfx.h>
#include <bx/bx.h>
#include <bx/error.h>
#include <bx/readerwriter.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace Caesura {

namespace {

constexpr uint8_t kUniformFragmentBit = 0x10;

// ---------------------------------------------------------------------------
// Build a bgfx shader binary (VSH/FSH/CSH v11) around raw backend code
// (DXBC for D3D11, GLSL source text for GL). Mirrors
// BgfxShaderManager::buildBgfxShader but supports compute magic and an
// explicit uniform table (D3D11 cbuffer layout = regIndex * 16 bytes).
// ---------------------------------------------------------------------------
struct ShaderUniformMeta {
    const char* name;
    uint8_t type;  // raw type byte (Vec4, optional fragment bit)
    uint8_t num;
    uint16_t regIndex;
    uint16_t regCount;
};

bgfx::ShaderHandle buildShaderBinary(const uint8_t* code, uint32_t codeSize,
                                     char type,
                                     const ShaderUniformMeta* uniforms,
                                     uint32_t uniformCount,
                                     uint8_t numAttrs,
                                     const uint16_t* attrIds) {
    if (!code || codeSize == 0 || codeSize > 65536) {
        DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                  "[SmaMeshRenderer] Shader rejected: %u bytes", codeSize);
        return BGFX_INVALID_HANDLE;
    }
    uint32_t uniformBytes = 0;
    uint32_t cbSize = 0;  // D3D11 per-shader constant buffer bytes
    for (uint32_t i = 0; i < uniformCount; ++i) {
        uniformBytes += 1 + static_cast<uint32_t>(std::strlen(uniforms[i].name))
                      + 1 + 1 + 2 + 2 + 2 + 2;
        cbSize += uint32_t(uniforms[i].regCount) * 16;
    }
    const uint32_t totalSize = 4 + 4 + 4 + 2 + uniformBytes + 4 + codeSize + 1
                             + 1 + 2 * numAttrs + 2;
    const bgfx::Memory* mem = bgfx::alloc(totalSize);
    if (!mem) return BGFX_INVALID_HANDLE;
    bx::StaticMemoryBlockWriter writer(mem->data, mem->size);
    bx::ErrorAssert err;
    const uint32_t magic = BX_MAKEFOURCC(type, 'S', 'H', 0x0B);
    bx::write(&writer, magic, err);
    bx::write(&writer, uint32_t(0), err);  // hashIn
    bx::write(&writer, uint32_t(0), err);  // hashOut
    bx::write(&writer, uint16_t(uniformCount), err);
    for (uint32_t i = 0; i < uniformCount; ++i) {
        const ShaderUniformMeta& u = uniforms[i];
        const uint8_t nameSize = uint8_t(std::strlen(u.name));
        bx::write(&writer, nameSize, err);
        bx::write(&writer, u.name, nameSize, err);
        bx::write(&writer, u.type, err);
        bx::write(&writer, u.num, err);
        bx::write(&writer, u.regIndex, err);
        bx::write(&writer, u.regCount, err);
        bx::write(&writer, uint16_t(0), err);  // texInfo
        bx::write(&writer, uint16_t(0), err);  // texFormat
    }
    bx::write(&writer, codeSize, err);
    bx::write(&writer, code, codeSize, err);
    bx::write(&writer, uint8_t(0), err);
    bx::write(&writer, numAttrs, err);
    for (uint8_t i = 0; i < numAttrs; ++i) bx::write(&writer, attrIds[i], err);
    // D3D11: the per-shader constant buffer byte size (0 = no cbuffer).
    bx::write(&writer, uint16_t(cbSize), err);
    return bgfx::createShader(mem);
}

struct Bytecode {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// init / lifecycle
// ---------------------------------------------------------------------------

SmaMeshRenderer::SmaMeshRenderer() = default;

SmaMeshRenderer::~SmaMeshRenderer() {
    if (m_listenerRegistered) {
        BackendRegistry::instance().unregisterDeviceLostListener(this);
        m_listenerRegistered = false;
    }
    releaseGpuResources();
}

void SmaMeshRenderer::releaseMeshGpuResources(MeshEntry& entry) {
    if (bgfx::isValid(entry.ib)) bgfx::destroy(entry.ib);
    if (bgfx::isValid(entry.gpuIn)) bgfx::destroy(entry.gpuIn);
    entry.ib = BGFX_INVALID_HANDLE;
    entry.gpuIn = BGFX_INVALID_HANDLE;
    entry.gpuReady = false;
    entry.gpuSkinReady = false;
}

void SmaMeshRenderer::releaseGpuResources() {
    for (auto& entry : m_meshes) {
        releaseMeshGpuResources(entry);
    }
    if (bgfx::isValid(m_skinProgram)) bgfx::destroy(m_skinProgram);
    m_skinProgram = BGFX_INVALID_HANDLE;
    m_shaders.reset();
    m_initialized = false;
    m_skinWarningShown = false;
}

void SmaMeshRenderer::init() {
    if (m_initialized) return;
    // bgfx not up (headless / CI): stay inert; every op becomes a no-op.
    if (bgfx::getRendererType() == bgfx::RendererType::Noop) return;

    m_layout
        .begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    // Compute input layout: pos(2) + uv(2) + bone0/bone1(2) + w0/w1(2).
    m_skinLayout
        .begin()
        .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord1, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord2, 2, bgfx::AttribType::Float)
        .end();

    m_shaders = std::make_unique<BgfxShaderManager>();
    m_shaders->initEmbeddedShaders();

    // S5: build the compute skin pass + the skin draw program when the
    // backend supports compute (D3D11/D3D12 and GL 4.3+).
    const bgfx::RendererType::Enum renderer = bgfx::getRendererType();
    const bool isD3D = renderer == bgfx::RendererType::Direct3D11
                    || renderer == bgfx::RendererType::Direct3D12;
    const bool isGL = renderer == bgfx::RendererType::OpenGL
                   || renderer == bgfx::RendererType::OpenGLES;
    const uint64_t caps = bgfx::getCaps()->supported;

    if ((caps & BGFX_CAPS_COMPUTE) && (isD3D || isGL)) {
        Bytecode cs = isD3D
            ? Bytecode{ kEmbeddedCS_SkinDXBC, kEmbeddedCS_SkinDXBC_size }
            : Bytecode{ kEmbeddedCS_SkinGL, kEmbeddedCS_SkinGL_size };
        bgfx::ShaderHandle csh = buildShaderBinary(
            cs.data, uint32_t(cs.size), 'C', nullptr, 0, 0, nullptr);
        if (bgfx::isValid(csh)) {
            m_skinProgram = bgfx::createProgram(csh, true);
        }
        // Every dispatch owns one immutable snapshot of poses and draw data.
        // Two metadata float4 rows precede all uint16-addressable pose rows.
        m_boneLayout.begin().add(bgfx::Attrib::TexCoord0, 4,
                                 bgfx::AttribType::Float).end();

        // The compute skin pass writes the FINAL NDC positions (draw
        // transform + view size ride in snapshot rows 0/1), so the
        // draw reuses the engine's proven passthrough program — no extra
        // vertex shader or uniforms needed.
        if (!bgfx::isValid(m_skinProgram)) {
            DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                      "[SmaMeshRenderer] S5 GPU skinning unavailable "
                      "(compute program build failed); using CPU skinner.");
        }
    }

    m_initialized = true;
    if (!m_listenerRegistered) {
        BackendRegistry::instance().registerDeviceLostListener(this);
        m_listenerRegistered = true;
    }
}

void SmaMeshRenderer::onDeviceLost() {
    releaseGpuResources();
}

void SmaMeshRenderer::onDeviceRestored() {
    init();
    if (!m_initialized) return;
    for (auto& entry : m_meshes) {
        uploadMeshGpuResources(entry);
    }
}

void SmaMeshRenderer::setSkinMode(SkinMode mode) {
    m_skinMode = mode;
}

SmaMeshRenderer::MeshEntry* SmaMeshRenderer::find(MeshHandle handle) {
    for (auto& entry : m_meshes) {
        if (entry.handle == handle) return &entry;
    }
    return nullptr;
}

bool SmaMeshRenderer::useGpuSkin(const MeshEntry& entry) const {
    if (m_skinMode == SkinMode::Cpu) return false;
    const bool capable = entry.gpuSkinReady
        && bgfx::isValid(m_skinProgram);
    if (m_skinMode == SkinMode::Gpu && !capable) {
        if (!m_skinWarningShown) {
            DEBUG_ERR(SubSys::Render, ErrCode::Ok,
                      "[SmaMeshRenderer] GPU skinning requested but "
                      "unavailable; falling back to CPU.");
            m_skinWarningShown = true;
        }
        return false;
    }
    return capable;  // Auto: capability-based
}

void SmaMeshRenderer::uploadMeshGpuResources(MeshEntry& entry) {
    releaseMeshGpuResources(entry);
    entry.ib = bgfx::createIndexBuffer(
        bgfx::copy(entry.mesh.indices.data(),
                   static_cast<uint32_t>(entry.mesh.indices.size() * sizeof(uint16_t))));
    entry.gpuReady = bgfx::isValid(entry.ib);

    const uint64_t caps = bgfx::getCaps()->supported;
    if (entry.gpuReady && (caps & BGFX_CAPS_COMPUTE)
        && bgfx::isValid(m_skinProgram)) {
        const uint32_t vcount = static_cast<uint32_t>(entry.mesh.vertices.size());
        std::vector<float> data;
        data.reserve(vcount * 8);
        for (const SMAMeshVertex& v : entry.mesh.vertices) {
            data.push_back(v.x);
            data.push_back(v.y);
            data.push_back(v.u);
            data.push_back(v.v);
            data.push_back(static_cast<float>(v.bone0));
            data.push_back(static_cast<float>(v.bone1));
            data.push_back(v.w0);
            data.push_back(v.w1);
        }
        entry.gpuIn = bgfx::createVertexBuffer(
            bgfx::copy(data.data(),
                       static_cast<uint32_t>(data.size() * sizeof(float))),
            m_skinLayout, BGFX_BUFFER_COMPUTE_READ);
        entry.gpuSkinReady = bgfx::isValid(entry.gpuIn);
    }
}

// ---------------------------------------------------------------------------
// IMeshRenderer
// ---------------------------------------------------------------------------

MeshHandle SmaMeshRenderer::createMesh(const SMAMesh& mesh) {
    if (!m_initialized) init();
    if (!m_initialized) return {};  // deferred-gpu: no GPU -> invalid handle
    if (mesh.vertices.empty() || mesh.indices.empty()
        || mesh.indices.size() % 3 != 0) {
        return {};
    }

    MeshEntry entry;
    entry.handle = MeshHandle{ m_nextId++ };
    entry.mesh = mesh;
    // CPU side: initial skinned copy = identity pose (raw vertices).
    entry.skinned.resize(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const SMAMeshVertex& v = mesh.vertices[i];
        entry.skinned[i] = { v.x, v.y, v.u, v.v };
    }
    // S5: compute input/output buffers when the skin pipeline is usable.
    // The mesh INPUT is immutable: its topology/weights are uploaded once.
    uploadMeshGpuResources(entry);

    m_meshes.push_back(std::move(entry));
    return m_meshes.back().handle;
}

void SmaMeshRenderer::destroyMesh(MeshHandle handle) {
    for (auto it = m_meshes.begin(); it != m_meshes.end(); ++it) {
        if (it->handle == handle) {
            releaseMeshGpuResources(*it);
            m_meshes.erase(it);
            return;
        }
    }
}

void SmaMeshRenderer::updateMesh(MeshHandle handle,
                                 const std::vector<BonePose>& poses) {
    MeshEntry* entry = find(handle);
    if (!entry || !entry->gpuReady) return;
    entry->pendingPoses = poses;
    if (useGpuSkin(*entry)) {
        // Defer the GPU work to drawMesh (same-frame order: the bone
        // buffer upload + dispatch must run in the draw's view, before
        // its submit). Store the poses for packing at draw time.
        return;
    }
    skinMesh(entry->mesh, poses, entry->skinned);
}

SmaMeshRenderer::GpuDrawPacket::~GpuDrawPacket() {
    // Both destruction commands execute after queued dispatches AND draws.
    // No later draw can reuse these resources before their submitted frame.
    if (bgfx::isValid(output)) bgfx::destroy(output);
    if (bgfx::isValid(snapshot)) bgfx::destroy(snapshot);
}

void SmaMeshRenderer::skinOnGpu(MeshEntry& entry,
                                const std::vector<BonePose>& poses,
                                uint16_t targetView,
                                float x, float y, float scale,
                                float viewW, float viewH, GpuDrawPacket& packet) {
    // Preserve every pose addressable by the interface's uint16 bone indices.
    // Missing rows remain missing, not identity padding or a 64-bone clamp.
    const uint32_t poseCount = static_cast<uint32_t>(
        std::min(poses.size(), size_t(UINT16_MAX) + 1));
    std::vector<float> packed((size_t(poseCount) + 2) * 4, 0.f);
    packed[0] = x; packed[1] = y; packed[2] = scale;
    packed[4] = viewW; packed[5] = viewH;
    const uint32_t vcount = static_cast<uint32_t>(entry.mesh.vertices.size());
    static_assert(sizeof(vcount) == sizeof(packed[0]));
    std::memcpy(&packed[6], &vcount, sizeof(vcount));
    std::memcpy(&packed[7], &poseCount, sizeof(poseCount));
    for (uint32_t i = 0; i < poseCount; ++i) packBonePose(poses[i], &packed[(size_t(i) + 2) * 4]);
    // Creation is queued before draws. bgfx::copy owns the upload bytes;
    // no later actor can overwrite this dispatch's poses or transform.
    packet.snapshot = bgfx::createVertexBuffer(
        bgfx::copy(packed.data(), static_cast<uint32_t>(packed.size() * sizeof(float))),
        m_boneLayout, BGFX_BUFFER_COMPUTE_READ);
    if (!bgfx::isValid(packet.snapshot)) {
        throw std::runtime_error("SMA GPU bone snapshot allocation failed");
    }
    packet.output = bgfx::createDynamicVertexBuffer(vcount, m_layout, BGFX_BUFFER_COMPUTE_WRITE);
    if (!bgfx::isValid(packet.output)) throw std::runtime_error("SMA GPU draw output allocation failed");
    const uint32_t numGroups = vcount / 64 + (vcount % 64 != 0);
    bgfx::setBuffer(0, entry.gpuIn, bgfx::Access::Read);
    bgfx::setBuffer(1, packet.snapshot, bgfx::Access::Read);
    bgfx::setBuffer(2, packet.output, bgfx::Access::Write);
    bgfx::dispatch(targetView, m_skinProgram, numGroups, 1, 1);
}

void SmaMeshRenderer::drawMesh(uint16_t targetView, MeshHandle handle,
                               uint32_t dstTexId, float x, float y,
                               float scale, float opacity) {
    if (!m_initialized) init();
    if (!m_initialized) return;
    MeshEntry* entry = find(handle);
    if (!entry || !entry->gpuReady) return;

    const bgfx::TextureHandle tex = { static_cast<uint16_t>(dstTexId) };
    if (!bgfx::isValid(tex)) return;

    // Plain fs_texture ignores BlendParams. The existing straight-alpha
    // modulated program applies per-actor opacity on both skinning paths.
    const auto program = m_shaders->getModulatedTextureProgram();
    if (!bgfx::isValid(program)) return;
    const float color[4] = {1.f, 1.f, 1.f, opacity};

    const uint32_t idxCount = static_cast<uint32_t>(entry->mesh.indices.size());

    const uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
        | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                BGFX_STATE_BLEND_INV_SRC_ALPHA);

    if (useGpuSkin(*entry)) {
        // Match the CPU admission contract before queuing compute work or
        // copying indices. bgfx may return less storage than requested.
        if (bgfx::getAvailTransientIndexBuffer(idxCount) < idxCount) return;
        // Skin + NDC transform on the GPU (dispatch in THIS view, before
        // the draw submit that consumes the output buffer).
        const bgfx::Stats* stats = bgfx::getStats();
        const float sw = stats ? static_cast<float>(stats->width) : 1280.f;
        const float sh = stats ? static_cast<float>(stats->height) : 720.f;
        if (sw <= 0.f || sh <= 0.f) return;
        const uint32_t vertCount =
            static_cast<uint32_t>(entry->mesh.vertices.size());
        bgfx::TransientIndexBuffer tib;
        bgfx::allocTransientIndexBuffer(&tib, idxCount);
        if (!tib.data || tib.size < size_t(idxCount) * sizeof(uint16_t)) return;
        std::memcpy(tib.data, entry->mesh.indices.data(),
                    idxCount * sizeof(uint16_t));
        // Final NDC also depends on draw transform and view size. Rebuild it
        // for every draw, retaining each draw's distinct output through submit.
        GpuDrawPacket packet;
        skinOnGpu(*entry, entry->pendingPoses, targetView, x, y, scale, sw, sh, packet);
        bgfx::setVertexBuffer(0, packet.output, 0, vertCount);
        bgfx::setIndexBuffer(&tib);
        bgfx::setState(state);
        bgfx::setTexture(0, m_shaders->getDefaultSampler(), tex);
        bgfx::setUniform(m_shaders->getColorUniform(), color);
        bgfx::submit(targetView, program);
        return;
    }

    // CPU path (S2): transient VB with the pixel->NDC transform applied.
    const uint32_t vertCount = static_cast<uint32_t>(entry->skinned.size());
    if (bgfx::getAvailTransientVertexBuffer(vertCount, m_layout) < vertCount) return;
    if (bgfx::getAvailTransientIndexBuffer(idxCount) < idxCount) return;

    const bgfx::Stats* stats = bgfx::getStats();
    const float sw = stats ? static_cast<float>(stats->width) : 1280.f;
    const float sh = stats ? static_cast<float>(stats->height) : 720.f;
    if (sw <= 0.f || sh <= 0.f) return;

    bgfx::TransientVertexBuffer tvb;
    bgfx::allocTransientVertexBuffer(&tvb, vertCount, m_layout);
    auto* verts = reinterpret_cast<SmaSkinnedVertex*>(tvb.data);
    for (uint32_t i = 0; i < vertCount; ++i) {
        const float px = x + entry->skinned[i].x * scale;
        const float py = y + entry->skinned[i].y * scale;
        verts[i].x = (px / sw) * 2.0f - 1.0f;
        verts[i].y = 1.0f - (py / sh) * 2.0f;
        verts[i].u = entry->skinned[i].u;
        verts[i].v = entry->skinned[i].v;
    }

    bgfx::TransientIndexBuffer tib;
    bgfx::allocTransientIndexBuffer(&tib, idxCount);
    std::memcpy(tib.data, entry->mesh.indices.data(),
                idxCount * sizeof(uint16_t));

    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    bgfx::setState(state);
    bgfx::setTexture(0, m_shaders->getDefaultSampler(), tex);
    bgfx::setUniform(m_shaders->getColorUniform(), color);
    bgfx::submit(targetView, program);
}

} // namespace Caesura
