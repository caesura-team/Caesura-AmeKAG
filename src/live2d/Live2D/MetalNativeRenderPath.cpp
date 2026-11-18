#if defined(CAESURA_LIVE2D) && defined(__APPLE__)

#include "MetalNativeRenderPath.h"
#include "debug/api/DebugLog.h"

#include <bgfx/platform.h>
#include <Model/CubismUserModel.hpp>
#include <Rendering/Metal/CubismDeviceInfo_Metal.hpp>
#include <Rendering/Metal/CubismRenderer_Metal.hpp>
#include <cstring>
#include <unordered_map>
#include <vector>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

namespace Caesura {

using CsmRendering::CubismDeviceInfo_Metal;
using CsmRendering::CubismRenderer;
using CsmRendering::CubismRenderer_Metal;

struct MetalNativeRenderPath::State {
    struct ModelResources {
        // The SDK stores non-owning Metal texture handles for the model.
        std::vector<id<MTLTexture>> textures;
        id<MTLTexture> color = nil;
        id<MTLTexture> depth = nil;
        id<MTLBuffer> readback = nil;
        size_t rowPitch = 0;
    };

    bool ensureTarget(ModelResources& model) const {
        if (model.color && model.color.width == width && model.color.height == height) {
            return true;
        }
        auto* colorDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:width height:height mipmapped:NO];
        colorDescriptor.usage = MTLTextureUsageRenderTarget;
        colorDescriptor.storageMode = MTLStorageModePrivate;
        id<MTLTexture> color = [device newTextureWithDescriptor:colorDescriptor];

        auto* depthDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
            width:width height:height mipmapped:NO];
        depthDescriptor.usage = MTLTextureUsageRenderTarget;
        depthDescriptor.storageMode = MTLStorageModePrivate;
        id<MTLTexture> depth = [device newTextureWithDescriptor:depthDescriptor];

        // Align the GPU-to-CPU copy independently of bgfx's packed rows.
        const size_t rowPitch = (width * 4 + 255) & ~size_t(255);
        id<MTLBuffer> readback = [device newBufferWithLength:rowPitch * height
                                      options:MTLResourceStorageModeShared];
        if (!color || !depth || !readback) return false;
        model.color = color;
        model.depth = depth;
        model.readback = readback;
        model.rowPitch = rowPitch;
        return true;
    }

    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLCommandBuffer> command = nil;
    CubismRenderer* activeRenderer = nullptr;
    size_t width = 0;
    size_t height = 0;
    bool reportedCommandFailure = false;
    std::unordered_map<CubismRenderer*, ModelResources> models;
};

MetalNativeRenderPath::MetalNativeRenderPath() : m_state(std::make_unique<State>()) {}
MetalNativeRenderPath::~MetalNativeRenderPath() = default;

bool MetalNativeRenderPath::init(int width, int height) {
    @autoreleasepool {
        const auto* internal = bgfx::getInternalData();
        if (!internal || !internal->context) {
            DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/Metal] bgfx Metal device is unavailable");
            return false;
        }
        NSBundle* bundle = [NSBundle mainBundle];
        for (NSString* name in @[@"MetalShaders", @"VertShaderSrcBlend", @"VertShaderSrcMaskedBlend"]) {
            if (![bundle URLForResource:name withExtension:@"metallib" subdirectory:@"FrameworkMetallibs"]) {
                DEBUG_ERR(SubSys::Live2D, ErrCode::Ok,
                    "[Live2D/Metal] Missing Cubism shader: FrameworkMetallibs/%s.metallib", name.UTF8String);
                return false;
            }
        }
        id<MTLDevice> device = (__bridge id<MTLDevice>)internal->context;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) {
            DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/Metal] Could not create a command queue");
            return false;
        }
        m_state->device = device;
        m_state->queue = queue;
        resize(width, height);
        CubismRenderer_Metal::SetConstantSettings(device, 1);
        return true;
    }
}

void MetalNativeRenderPath::shutdown() {
    auto& state = *m_state;
    state.command = nil;
    state.activeRenderer = nullptr;
    state.models.clear();
    if (state.device) CubismDeviceInfo_Metal::ReleaseDeviceInfo(state.device);
    state.queue = nil;
    state.device = nil;
}

CubismRenderer* MetalNativeRenderPath::createModelRenderer(
    Live2D::Cubism::Framework::CubismUserModel& model, int width, int height) {
    @autoreleasepool {
        model.CreateRenderer(width, height);
        return model.GetRenderer<CubismRenderer_Metal>();
    }
}

bool MetalNativeRenderPath::bindModelTexture(CubismRenderer* renderer, std::uint32_t index,
                                            int width, int height, const unsigned char* pixels) {
    @autoreleasepool {
        auto* descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:width height:height mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModeManaged;
        if (@available(macOS 10.15, *)) {
            if (m_state->device.hasUnifiedMemory) descriptor.storageMode = MTLStorageModeShared;
        }
        id<MTLTexture> texture = [m_state->device newTextureWithDescriptor:descriptor];
        if (!texture) return false;
        [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                    mipmapLevel:0 withBytes:pixels bytesPerRow:size_t(width) * 4];
        auto& textures = m_state->models[renderer].textures;
        if (textures.size() <= index) textures.resize(size_t(index) + 1);
        textures[index] = texture;
        static_cast<CubismRenderer_Metal*>(renderer)->BindTexture(index, texture);
        return true;
    }
}

void MetalNativeRenderPath::beginFrame(CubismRenderer* renderer) {
    @autoreleasepool {
        auto& state = *m_state;
        state.command = nil;
        state.activeRenderer = nullptr;
        if (!renderer || !state.queue || !state.width || !state.height) return;
        auto& model = state.models[renderer];
        if (!state.ensureTarget(model)) return;
        id<MTLCommandBuffer> command = [state.queue commandBuffer];
        if (!command) return;

        auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = model.color;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.depthAttachment.texture = model.depth;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.clearDepth = 1.0;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;

        auto* metal = static_cast<CubismRenderer_Metal*>(renderer);
        metal->StartFrame(command, pass);
        metal->DrawModel();
        state.command = command;
        state.activeRenderer = renderer;
    }
}

void MetalNativeRenderPath::endFrame(CubismRenderer* renderer, bgfx::TextureHandle bgfxTex) {
    @autoreleasepool {
        auto& state = *m_state;
        if (state.activeRenderer != renderer || !state.command) return;
        id<MTLCommandBuffer> command = state.command;
        state.command = nil;
        state.activeRenderer = nullptr;
        if (!bgfx::isValid(bgfxTex)) return;

        const auto& model = state.models.at(renderer);
        const size_t rows = state.height;
        const size_t bytesPerRow = state.width * 4;
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!blit) return;
        [blit copyFromTexture:model.color sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(state.width, rows, 1)
                     toBuffer:model.readback destinationOffset:0
        destinationBytesPerRow:model.rowPitch destinationBytesPerImage:model.rowPitch * rows];
        [blit endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            if (!state.reportedCommandFailure) {
                const char* reason = command.error.localizedDescription.UTF8String;
                DEBUG_ERR(SubSys::Live2D, ErrCode::Ok, "[Live2D/Metal] Drawing failed: %s",
                    reason ? reason : "no Metal error description");
                state.reportedCommandFailure = true;
            }
            return;
        }
        state.reportedCommandFailure = false;

        const auto* memory = bgfx::alloc(static_cast<std::uint32_t>(bytesPerRow * rows));
        const auto* source = static_cast<const unsigned char*>(model.readback.contents);
        for (size_t row = 0; row < rows; ++row) {
            std::memcpy(memory->data + row * bytesPerRow, source + row * model.rowPitch, bytesPerRow);
        }
        bgfx::updateTexture2D(bgfxTex, 0, 0, 0, 0, static_cast<std::uint16_t>(state.width),
                              static_cast<std::uint16_t>(rows), memory);
    }
}

void MetalNativeRenderPath::releaseModelTarget(CubismRenderer* renderer) {
    m_state->models.erase(renderer);
}

void MetalNativeRenderPath::resize(int width, int height) {
    // Keep the resource sizes representable in both Metal and bgfx's API.
    constexpr int maxTextureDimension = 16384;
    if (width <= 0 || height <= 0 || width > maxTextureDimension || height > maxTextureDimension) return;
    m_state->width = static_cast<size_t>(width);
    m_state->height = static_cast<size_t>(height);
}

} // namespace Caesura

#endif
