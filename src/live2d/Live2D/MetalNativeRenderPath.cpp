// MetalNativeRenderPath.cpp — complete macOS Metal render path for Live2D
// (C1/R1 closure). Renders each model to a Cubism offscreen target, reads
// the MTLTexture back synchronously, and uploads it into the bgfx texture
// the engine composites (same contract as OpenGLReadbackRenderPath).
// Compiles only with CAESURA_LIVE2D + __APPLE__ (Objective-C++).
#if defined(CAESURA_LIVE2D) && defined(__APPLE__)

#include "MetalNativeRenderPath.h"
#include <SDL3/SDL.h>
#include "debug/api/DebugLog.h"

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include <Rendering/CubismRenderer.hpp>
#include <Rendering/Metal/CubismRenderer_Metal.hpp>
#include <Rendering/Metal/CubismOffscreenRenderTarget_Metal.hpp>

#import <Metal/Metal.h>

namespace Caesura {

using namespace Csm;
using namespace Csm::Rendering;

static id<MTLDevice> getBgfxMetalDevice() {
    const bgfx::InternalData* internal = bgfx::getInternalData();
    if (!internal) return nil;
    return (__bridge id<MTLDevice>)internal->context;
}

bool MetalNativeRenderPath::init(int width, int height) {
    m_device = getBgfxMetalDevice();
    if (!m_device) {
        DEBUG_WARN(SubSys::Live2D, ErrCode::Ok,
            "[Live2D/Metal] bgfx MTLDevice not available, falling back");
        return false;
    }
    m_width = width;
    m_height = height;

    // Required before any CubismRenderer_Metal::CreateRenderer() (model load).
    CubismRenderer_Metal::SetConstantSettings(m_device, 1);

    m_commandQueue = [m_device newCommandQueue];
    if (!m_commandQueue) {
        DEBUG_WARN(SubSys::Live2D, ErrCode::Ok,
            "[Live2D/Metal] newCommandQueue failed, falling back");
        return false;
    }

    DEBUG_INFO(SubSys::Live2D, ErrCode::Ok, "[Live2D/Metal] Render path ready — shared device (bgfx Metal)");
    return true;
}

void MetalNativeRenderPath::shutdown() {
    m_targets.clear();  // offscreen targets are owned by their renderer pair
    m_commandQueue = nil;
    m_device = nil;
}

// Per-model offscreen target (Cubism renders here; endFrame reads it back).
bool MetalNativeRenderPath::ensureTarget(CsmRendering::CubismRenderer* renderer) {
    auto it = m_targets.find(renderer);
    if (it != m_targets.end() && it->second.target.GetColorBuffer() != nil) {
        return true;
    }
    OffscreenEntry entry;
    entry.target.SetOffscreenRenderTarget(m_device, nullptr, m_width, m_height);
    m_targets[renderer] = std::move(entry);
    return true;
}

void MetalNativeRenderPath::beginFrame(CsmRendering::CubismRenderer* renderer) {
    if (!renderer) return;
    ensureTarget(renderer);
    auto it = m_targets.find(renderer);
    if (it == m_targets.end()) return;

    id<MTLTexture> color = it->second.target.GetColorBuffer();
    MTLRenderPassDescriptor* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    descriptor.colorAttachments[0].texture = color;
    descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
    descriptor.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLCommandBuffer> cmd = [m_commandQueue commandBuffer];
    if (cmd) {
        auto* metal = static_cast<CubismRenderer_Metal*>(renderer);
        metal->StartFrame(cmd, descriptor);
        m_activeCommand = cmd;
    }
}

void MetalNativeRenderPath::endFrame(CsmRendering::CubismRenderer* renderer,
                                     bgfx::TextureHandle bgfxTex) {
    if (!renderer) return;
    auto it = m_targets.find(renderer);
    if (it == m_targets.end()) return;

    // Commit the command buffer and wait: the readback below needs the
    // render to be complete (synchronous path, same as the GL readback).
    id<MTLCommandBuffer> cmd = m_activeCommand;
    m_activeCommand = nil;
    if (cmd) {
        [cmd commit];
        [cmd waitUntilCompleted];
    }

    id<MTLTexture> color = it->second.target.GetColorBuffer();
    if (!color || !bgfx::isValid(bgfxTex)) return;

    const uint32_t w = m_width;
    const uint32_t h = m_height;
    std::vector<uint8_t>& pixels = it->second.pixels;
    if (pixels.size() < size_t(w) * h * 4) pixels.resize(size_t(w) * h * 4);
    [color getBytes:pixels.data()
          bytesPerRow:w * 4
           fromRegion:MTLRegionMake2D(0, 0, w, h)
          mipmapLevel:0];

    const bgfx::Memory* mem = bgfx::copy(pixels.data(), w * h * 4);
    bgfx::updateTexture2D(bgfxTex, 0, 0, 0, 0, w, h, mem);
}

void MetalNativeRenderPath::releaseModelTarget(CsmRendering::CubismRenderer* renderer) {
    m_targets.erase(renderer);
}

void MetalNativeRenderPath::resize(int width, int height) {
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;
    m_targets.clear();  // recreated on next beginFrame at the new size
}

} // namespace Caesura

#endif // CAESURA_LIVE2D && __APPLE__
