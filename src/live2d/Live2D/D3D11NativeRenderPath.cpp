#if defined(CAESURA_LIVE2D) && defined(_WIN32)

#include "D3D11NativeRenderPath.h"
#include "debug/api/DebugLog.h"

// bgfx internals
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

// Cubism
#include <Rendering/CubismRenderer.hpp>
#include <Rendering/D3D11/CubismRenderer_D3D11.hpp>
#include <Rendering/D3D11/CubismDeviceInfo_D3D11.hpp>
#include <Rendering/D3D11/CubismShader_D3D11.hpp>

// Direct3D 11
#include <d3d11.h>

#include <SDL3/SDL.h>

namespace Caesura {

using namespace Csm;
using namespace Csm::Rendering;

// ============================================================
// Get bgfx's D3D11 device
// ============================================================
static ID3D11Device* getBgfxD3D11Device() {
    const bgfx::InternalData* internal = bgfx::getInternalData();
    if (!internal) return nullptr;
    return static_cast<ID3D11Device*>(internal->context);
}

// ============================================================
// init / shutdown
// ============================================================
bool D3D11NativeRenderPath::retainLoadedRuntime() {
    if (m_runtimeModule) return true;
    HMODULE module = nullptr;
    // bgfx already selected and loaded this runtime. Take a normal counted
    // reference; do not load a replacement DLL or permanently pin it.
    if (!GetModuleHandleExW(0, L"d3d11.dll", &module)) return false;
    m_runtimeModule = module;
    return true;
}

bool D3D11NativeRenderPath::init(int width, int height) {
    if (m_context) return true;
    if (!retainLoadedRuntime()) return false;
    m_device = getBgfxD3D11Device();
    if (!m_device) {
        DEBUG_WARN(SubSys::Live2D, ErrCode::Ok,
            "[Live2D/D3D11] bgfx D3D11 device not available, falling back");
        shutdown();
        return false;
    }
    m_device->GetImmediateContext(&m_context);  // AddRef balanced in shutdown.
    if (!m_context) {
        shutdown();
        return false;
    }
    m_width = width;
    m_height = height;

    // Required before any CubismRenderer_D3D11::CreateRenderer() (model load).
    CubismRenderer_D3D11::SetConstantSettings(1, m_device);

    DEBUG_INFO(SubSys::Live2D, ErrCode::Ok, "[Live2D/D3D11] Render path ready — shared device (bgfx D3D11)");
    return true;
}

bool D3D11NativeRenderPath::ensureShadersReady() {
    if (!m_device || !m_context || FAILED(m_device->GetDeviceRemovedReason())) {
        m_shaderReadiness = ShaderReadiness::Failed;
        return false;
    }
    if (m_shaderReadiness != ShaderReadiness::Unknown) {
        return m_shaderReadiness == ShaderReadiness::Ready;
    }
    // Cache failure too: the SDK exposes a void setup operation and may retain
    // a partial shader set. Never repeatedly compile or erase shared device
    // state that another renderer could reference.
    m_shaderReadiness = ShaderReadiness::Failed;
    auto* deviceInfo = CubismDeviceInfo_D3D11::GetDeviceInfo(m_device);
    auto* shaders = deviceInfo ? deviceInfo->GetShader() : nullptr;
    if (!shaders) return false;
    for (csmUint32 index = 0; index < static_cast<csmUint32>(ShaderNames_Max); ++index) {
        if (!shaders->GetVertexShader(index) || !shaders->GetPixelShader(index)) {
            DEBUG_ERR(SubSys::Live2D, ErrCode::Ok,
                "[Live2D/D3D11] Shader initialization incomplete at slot %u; model load refused", index);
            return false;
        }
    }

    // BindShader silently does nothing when its private input layout is null.
    // Probe from an empty IA binding, preserving bgfx's prior state and both
    // counted references acquired by IAGetInputLayout. No GPU work is submitted.
    ID3D11InputLayout* saved = nullptr;
    ID3D11InputLayout* actual = nullptr;
    m_context->IAGetInputLayout(&saved);
    m_context->IASetInputLayout(nullptr);
    shaders->BindShader(m_context);
    m_context->IAGetInputLayout(&actual);
    m_context->IASetInputLayout(saved);
    const bool hasLayout = actual != nullptr;
    if (actual) actual->Release();
    if (saved) saved->Release();
    if (!hasLayout) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok,
            "[Live2D/D3D11] Shader input layout unavailable; model load refused");
        return false;
    }
    m_shaderReadiness = ShaderReadiness::Ready;
    return true;
}

void D3D11NativeRenderPath::shutdown() {
    m_shaderReadiness = ShaderReadiness::Unknown;
    for (auto& [renderer, target] : m_targets) {
        (void)renderer;
        if (target.rtv) target.rtv->Release();
        if (target.tex) target.tex->Release();
    }
    m_targets.clear();
    // GetImmediateContext() AddRefs the returned context; balance it here.
    // The underlying device/context remain owned by bgfx.
    if (m_context) { m_context->Release(); m_context = nullptr; }
    m_device = nullptr;
    // Live2DBackend releases model and Cubism static COM owners before this
    // call. The loader reference must be the last D3D11 owner retired here.
    if (m_runtimeModule) {
        FreeLibrary(static_cast<HMODULE>(m_runtimeModule));
        m_runtimeModule = nullptr;
    }
}

// ============================================================
// Per-model render target (RTV for Cubism output → bgfx input via overrideInternal)
// ============================================================
bool D3D11NativeRenderPath::createModelTarget(CsmRendering::CubismRenderer* renderer,
                                              int width, int height) {
    auto it = m_targets.find(renderer);
    if (it != m_targets.end()) return true;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width  = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ModelTarget target;
    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &target.tex);
    if (FAILED(hr)) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/D3D11] CreateTexture2D failed: 0x%08X", hr);
        return false;
    }
    hr = m_device->CreateRenderTargetView(target.tex, nullptr, &target.rtv);
    if (FAILED(hr)) {
        // Partial-failure rollback (P2-2): free everything so the next frame retries.
        target.tex->Release();
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/D3D11] CreateRenderTargetView failed: 0x%08X", hr);
        return false;
    }

    m_targets.emplace(renderer, target);
    m_width = width;
    m_height = height;
    return true;
}

void D3D11NativeRenderPath::releaseModelTarget(CsmRendering::CubismRenderer* renderer) {
    auto it = m_targets.find(renderer);
    if (it == m_targets.end()) return;
    if (it->second.rtv) it->second.rtv->Release();
    if (it->second.tex) it->second.tex->Release();
    m_targets.erase(it);
}

// ============================================================
// Model texture (D3D11 SRV for CubismRenderer_D3D11::BindTexture)
// ============================================================
ID3D11ShaderResourceView* D3D11NativeRenderPath::createModelTexture(
    int width, int height, const unsigned char* pixels) {
    if (!m_device || width <= 0 || height <= 0 || !pixels) return nullptr;
    // D3D11 hardware texture dimension limit (defense in depth; the backend
    // already validates the header before decode).
    if (width > 16384 || height > 16384) return nullptr;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width  = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = pixels;
    initData.SysMemPitch = static_cast<UINT>(width) * 4;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = m_device->CreateTexture2D(&desc, &initData, &tex);
    if (FAILED(hr)) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok,
            "[Live2D/D3D11] CreateTexture2D (model) failed: 0x%08X", hr);
        return nullptr;
    }
    ID3D11ShaderResourceView* srv = nullptr;
    hr = m_device->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    if (FAILED(hr)) {
        DEBUG_ERR(SubSys::Live2D, ErrCode::Ok,
            "[Live2D/D3D11] CreateShaderResourceView (model) failed: 0x%08X", hr);
        return nullptr;
    }
    // Ownership passes to the caller (Live2DModel::textureSrvs), which
    // releases it on unload/destruction.
    return srv;
}

// ============================================================
// Per-frame: Cubism D3D11 → GPU copy → bgfx
// ============================================================
void D3D11NativeRenderPath::beginFrame(CubismRenderer* renderer) {
    // Each model renders into its own texture (RTV); bgfx consumes it via
    // overrideInternal in endFrame(). bgfx runs single-threaded
    // (BGFX_CONFIG_MULTITHREADED=0), so driving the shared D3D11 context here
    // is serialized with bgfx's own frame.
    auto* d3dRenderer = static_cast<CubismRenderer_D3D11*>(renderer);
    if (!d3dRenderer) return;

    ModelTarget* target = nullptr;
    auto it = m_targets.find(renderer);
    if (it == m_targets.end()) {
        if (!createModelTarget(renderer, m_width, m_height)) return;
        it = m_targets.find(renderer);
    }
    target = &it->second;

    ID3D11RenderTargetView* prevRTV = nullptr;
    ID3D11DepthStencilView* prevDSV = nullptr;
    m_context->OMGetRenderTargets(1, &prevRTV, &prevDSV);

    m_context->OMSetRenderTargets(1, &target->rtv, nullptr);
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_context->ClearRenderTargetView(target->rtv, clearColor);
    const D3D11_VIEWPORT viewport = { 0.0f, 0.0f,
        static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f };
    m_context->RSSetViewports(1, &viewport);

    d3dRenderer->StartFrame(m_context);
    d3dRenderer->DrawModel();
    d3dRenderer->EndFrame();

    // Restore bgfx's render target for the rest of this frame.
    m_context->OMSetRenderTargets(1, &prevRTV, prevDSV);
    if (prevRTV) prevRTV->Release();
    if (prevDSV) prevDSV->Release();
}

void D3D11NativeRenderPath::endFrame(CubismRenderer* renderer, bgfx::TextureHandle bgfxTex) {
    if (!bgfx::isValid(bgfxTex)) return;
    auto it = m_targets.find(renderer);
    if (it == m_targets.end() || !it->second.tex) return;

    // A new bgfx handle can still have a queued texture creation command.
    // Cache only a confirmed binding; a zero result must retry next frame.
    // Keep this per model so simultaneous models do not recreate each
    // other's shader-resource views on every frame.
    auto& target = it->second;
    if (target.boundBgfxTex.idx != bgfxTex.idx) {
        const auto nativeTexture = reinterpret_cast<uintptr_t>(target.tex);
        if (bgfx::overrideInternal(bgfxTex, nativeTexture) == nativeTexture) {
            target.boundBgfxTex = bgfxTex;
        }
    }
}

void D3D11NativeRenderPath::resize(int width, int height) {
    // Not wired to any caller yet; per-model targets are recreated lazily on
    // the next beginFrame if the size changes.
    m_width = width;
    m_height = height;
    for (auto& [renderer, target] : m_targets) {
        (void)renderer;
        if (target.rtv) target.rtv->Release();
        if (target.tex) target.tex->Release();
    }
    m_targets.clear();
}

} // namespace Caesura

#endif
