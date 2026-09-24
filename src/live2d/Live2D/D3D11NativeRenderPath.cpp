#if defined(CAESURA_LIVE2D) && defined(_WIN32)

#include "D3D11NativeRenderPath.h"
#include "debug/api/DebugLog.h"

// bgfx internals
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

// Cubism
#include <Rendering/CubismRenderer.hpp>
#include <Rendering/D3D11/CubismRenderer_D3D11.hpp>

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

static ID3D11DeviceContext* getBgfxD3D11Context() {
    ID3D11Device* device = getBgfxD3D11Device();
    if (!device) return nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    return ctx;
}

// ============================================================
// init / shutdown
// ============================================================
bool D3D11NativeRenderPath::init(int width, int height) {
    m_device = getBgfxD3D11Device();
    if (!m_device) {
        DEBUG_WARN(SubSys::Live2D, ErrCode::Ok,
            "[Live2D/D3D11] bgfx D3D11 device not available, falling back");
        return false;
    }
    m_device->GetImmediateContext(&m_context);  // shared with bgfx, no Release
    m_width = width;
    m_height = height;

    // Required before any CubismRenderer_D3D11::CreateRenderer() (model load).
    CubismRenderer_D3D11::SetConstantSettings(1, m_device);

    DEBUG_INFO(SubSys::Live2D, ErrCode::Ok, "[Live2D/D3D11] Render path ready — shared device (bgfx D3D11)");
    return true;
}

void D3D11NativeRenderPath::shutdown() {
    for (auto& [renderer, target] : m_targets) {
        (void)renderer;
        if (target.rtv) target.rtv->Release();
        if (target.tex) target.tex->Release();
    }
    m_targets.clear();
    m_lastOverriddenTex = nullptr;
    // GetImmediateContext() AddRefs the returned context; balance it here.
    // The underlying device/context remain owned by bgfx.
    if (m_context) { m_context->Release(); m_context = nullptr; }
    m_device = nullptr;
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
    m_lastOverriddenTex = nullptr;
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

    // Cubism already drew into this model's texture (bound in beginFrame);
    // hand it to bgfx. overrideInternal recreates the SRV every call, so only
    // do it when the texture actually changed (first frame / after resize).
    if (m_lastOverriddenTex != it->second.tex) {
        bgfx::overrideInternal(bgfxTex, reinterpret_cast<uintptr_t>(it->second.tex));
        m_lastOverriddenTex = it->second.tex;
    }
}

void D3D11NativeRenderPath::resize(int width, int height) {
    // Not wired to any caller yet; per-model targets are recreated lazily on
    // the next beginFrame if the size changes.
    m_width = width;
    m_height = height;
    m_lastOverriddenTex = nullptr;
    for (auto& [renderer, target] : m_targets) {
        (void)renderer;
        if (target.rtv) target.rtv->Release();
        if (target.tex) target.tex->Release();
    }
    m_targets.clear();
}

} // namespace Caesura

#endif
