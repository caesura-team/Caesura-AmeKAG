#pragma once

#include <bgfx/bgfx.h>
#include <string>

namespace Caesura {

enum class ShaderTestFault {
    None, FallbackMissingFragment, BlendMissingFragment,
    SoftBlurMissingFragment, TransitionMissingFragment
};

struct ShaderBuildReport {
    bool coreProgramsReady = false;
    int buildFailures = 0;
    unsigned injectedFaultCount = 0;
    ShaderTestFault requestedFault = ShaderTestFault::None;
    std::string failedPrograms;
};

class BgfxShaderManager {
public:
    BgfxShaderManager() = default;
    ~BgfxShaderManager();

    BgfxShaderManager(const BgfxShaderManager&) = delete;
    BgfxShaderManager& operator=(const BgfxShaderManager&) = delete;

    void initEmbeddedShaders();
    // Test source input, never a forged handle or mutation of readiness flags.
    // Non-test builds reject every non-None fault. Selection ends at first init.
    static bool testFaultsEnabled();
    static bool supportsTestFault(ShaderTestFault fault);
    bool setTestFault(ShaderTestFault fault);
    ShaderBuildReport buildReport() const;

    // -- t73: embedded-shader feeding contract ---------------------------------
    // Metal and desktop-GL embedded arrays are COMPLETE shaderc BGFX binaries
    // (VSH11/FSH11 fourcc at offset 0 with '#include'/'#version' source text in
    // the payload). They must go DIRECTLY to bgfx::createShader; re-wrapping
    // them inside buildBgfxShader nests a binary inside a binary and hands
    // "VSH<ver>..." to the MSL/GLSL compiler (t71 SIGABRT root cause).
    static bool usesDirectFeed(bool isMetal, bool isGL, bool isGLES);
    // Pure format probe (no GPU): true iff [data,size) is a text-coded BGFX
    // shader binary a renderer can consume directly (binary-in-binary inputs
    // and truncated buffers return false).
    static bool isDirectFeedBinary(const uint8_t* data, size_t size);
    int  shaderBuildFailures() const { return m_buildFailures; }
    bool coreProgramsBroken() const;

    bgfx::ProgramHandle getFallbackProgram()    const { return m_fallbackProgram; }
    bgfx::ProgramHandle getBlendProgram()       const { return m_blendProgram; }
    bgfx::ProgramHandle getTransitionProgram()  const { return m_transitionProgram; }
    bgfx::ProgramHandle getVFXProgram()         const { return m_vfxProgram; }
    bgfx::ProgramHandle getStretchProgram()     const { return m_stretchProgram; }
    bgfx::ProgramHandle getAffineProgram()      const { return m_affineProgram; }
    bgfx::UniformHandle getDefaultSampler() const {
        if (!bgfx::isValid(m_texSampler))
            m_texSampler = bgfx::createUniform("s_texture", bgfx::UniformType::Sampler);
        return m_texSampler;
    }
    bgfx::UniformHandle getSampler1() const {
        if (!bgfx::isValid(m_texSampler1))
            m_texSampler1 = bgfx::createUniform("s_texture1", bgfx::UniformType::Sampler);
        return m_texSampler1;
    }
    bgfx::UniformHandle getSampler2() const {
        if (!bgfx::isValid(m_texSampler2))
            m_texSampler2 = bgfx::createUniform("s_texture2", bgfx::UniformType::Sampler);
        return m_texSampler2;
    }
    bgfx::UniformHandle getBlendParams()        const { return m_u_blendParams; }
    bgfx::UniformHandle getTransParams()        const { return m_u_transParams; }
    bgfx::UniformHandle getVFXParams()          const { return m_u_vfxParams; }
    bgfx::UniformHandle getStretchParams()      const { return m_u_stretchParams; }
    bgfx::UniformHandle getAffineParams()       const { return m_u_affineParams; }
    // -- Round-102 post-processing chain ---------------------------------------
    // Per-kind full-screen PS programs driven by a shared PostFxParams uniform.
    bgfx::ProgramHandle getPostFxProgram(int kind) const {
        switch (kind) {
            case 0: return m_postfxVignette;   // Vignette
            case 1: return m_postfxLut;        // LutColorGrade
            case 2: return m_postfxBlur;       // SoftBlur
            case 3: return m_postfxBloom;      // Bloom
            case 4: return m_postfxLut3d;       // Lut3D (t214)
            default: return BGFX_INVALID_HANDLE;
        }
    }
    bgfx::UniformHandle getPostFxParams() const { return m_u_postfxParams; }

private:
    bgfx::ProgramHandle m_fallbackProgram    = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_blendProgram       = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_transitionProgram  = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_vfxProgram         = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_stretchProgram     = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_affineProgram      = BGFX_INVALID_HANDLE;
    mutable bgfx::UniformHandle m_texSampler  = BGFX_INVALID_HANDLE;
    mutable bgfx::UniformHandle m_texSampler1 = BGFX_INVALID_HANDLE;
    mutable bgfx::UniformHandle m_texSampler2 = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_u_blendParams      = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_u_transParams      = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_u_vfxParams        = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_u_stretchParams    = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_u_affineParams     = BGFX_INVALID_HANDLE;
    // Round-102 post-process programs + shared params uniform
    bgfx::ProgramHandle m_postfxVignette   = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_postfxLut        = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_postfxBlur       = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_postfxBloom      = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_postfxLut3d      = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_u_postfxParams   = BGFX_INVALID_HANDLE;
    int m_buildFailures = 0;  // t73: loud-degrade tally (see coreProgramsBroken)
    std::string m_failedProgramNames;  // t73: non-core failure tally for the one-shot ERROR
    bool m_embeddedInit = false;  // per-instance initEmbeddedShaders guard
    ShaderTestFault m_testFault = ShaderTestFault::None;
    unsigned m_injectedFaultCount = 0;
};

} // namespace Caesura
