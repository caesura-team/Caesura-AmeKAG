 #pragma once
#include <cstdint>
#include <cstddef>

namespace Caesura {

// ---------------------------------------------------------------------------
// Precompiled Vulkan SPIR-V bytecodes (Vulkan backend fallback)
// ---------------------------------------------------------------------------

extern const uint32_t kEmbeddedVS_Sprite[];
extern const size_t   kEmbeddedVS_SpriteSize;
extern const uint32_t kEmbeddedFS_Texture[];

// SPIR-V fullscreen effects (Vulkan)
extern const uint32_t kEmbeddedSPIRV_vs_fullscreen[];
extern const size_t   kEmbeddedSPIRV_vs_fullscreen_size;
extern const uint32_t kEmbeddedSPIRV_fs_blend[];
extern const size_t   kEmbeddedSPIRV_fs_blend_size;
extern const uint32_t kEmbeddedSPIRV_fs_transition[];
extern const size_t   kEmbeddedSPIRV_fs_transition_size;
extern const uint32_t kEmbeddedSPIRV_fs_vfx[];
extern const size_t   kEmbeddedSPIRV_fs_vfx_size;
extern const size_t   kEmbeddedFS_TextureSize;

// ---------------------------------------------------------------------------
// Precompiled DX11 DXBC bytecodes (Direct3D 11/12 backend fallback)
// Compiled from shaders/dx11/*.hlsl via fxc.exe
// ---------------------------------------------------------------------------

extern const uint8_t  kEmbeddedDXBC_VS_Sprite[];
extern const size_t   kEmbeddedDXBC_VS_Sprite_size;
extern const uint8_t  kEmbeddedDXBC_FS_Texture[];
extern const size_t   kEmbeddedDXBC_FS_Texture_size;
extern const uint8_t  kEmbeddedDXBC_fs_modulated_texture[];
extern const size_t   kEmbeddedDXBC_fs_modulated_texture_size;

// ---------------------------------------------------------------------------
// Precompiled DX11 DXBC bytecodes -- blend / transition / VFX / fullscreen
// ---------------------------------------------------------------------------

extern const uint8_t  kEmbeddedDXBC_vs_fullscreen[];
extern const size_t   kEmbeddedDXBC_vs_fullscreen_size;
extern const uint8_t  kEmbeddedDXBC_fs_blend[];
extern const size_t   kEmbeddedDXBC_fs_blend_size;
extern const uint8_t  kEmbeddedDXBC_fs_transition[];
extern const size_t   kEmbeddedDXBC_fs_transition_size;
extern const uint8_t  kEmbeddedDXBC_fs_vfx[];
extern const size_t   kEmbeddedDXBC_fs_vfx_size;

// -- SMA S5 GPU skinning shaders --------------------------------------------
// D3D11: DXBC compute (skin_cs.hlsl) + draw VS (skin_vs.hlsl); GL: raw
// GLSL 430 source text. See EmbeddedShaders_S5.cpp (auto-generated).
extern const uint8_t  kEmbeddedCS_SkinDXBC[];
extern const size_t   kEmbeddedCS_SkinDXBC_size;
extern const uint8_t  kEmbeddedVS_SkinDXBC[];
extern const size_t   kEmbeddedVS_SkinDXBC_size;
extern const uint8_t  kEmbeddedCS_SkinGL[];
extern const size_t   kEmbeddedCS_SkinGL_size;
extern const uint8_t  kEmbeddedVS_SkinGL[];
extern const size_t   kEmbeddedVS_SkinGL_size;

// -- Stretch / Affine blit shaders (DXBC, for D3D11/D3D12 backends) ----------
extern const uint8_t  kEmbeddedDXBC_stretch_blt_vs[];
extern const size_t   kEmbeddedDXBC_stretch_blt_vs_size;
extern const uint8_t  kEmbeddedDXBC_stretch_blt_fs[];
extern const size_t   kEmbeddedDXBC_stretch_blt_fs_size;
extern const uint8_t  kEmbeddedDXBC_affine_blt_vs[];
extern const size_t   kEmbeddedDXBC_affine_blt_vs_size;
extern const uint8_t  kEmbeddedDXBC_affine_blt_fs[];
extern const size_t   kEmbeddedDXBC_affine_blt_fs_size;


// ---------------------------------------------------------------------------
// Precompiled OpenGL GLSL bytecodes (OpenGL / OpenGLES backends)
// Compiled from shaders/glsl/*.sc via bgfx shaderc (--platform linux)
// ---------------------------------------------------------------------------
extern const uint8_t  kEmbeddedGL_vs_sprite[];
extern const size_t   kEmbeddedGL_vs_sprite_size;
extern const uint8_t  kEmbeddedGL_vs_fullscreen[];
extern const size_t   kEmbeddedGL_vs_fullscreen_size;
extern const uint8_t  kEmbeddedGL_stretch_blt_vs[];
extern const size_t   kEmbeddedGL_stretch_blt_vs_size;
extern const uint8_t  kEmbeddedGL_affine_blt_vs[];
extern const size_t   kEmbeddedGL_affine_blt_vs_size;
extern const uint8_t  kEmbeddedGL_fs_texture[];
extern const size_t   kEmbeddedGL_fs_texture_size;
extern const uint8_t  kEmbeddedGL_fs_modulated_texture[];
extern const size_t   kEmbeddedGL_fs_modulated_texture_size;
extern const uint8_t  kEmbeddedGL_fs_blend[];
extern const size_t   kEmbeddedGL_fs_blend_size;
extern const uint8_t  kEmbeddedGL_fs_transition[];
extern const size_t   kEmbeddedGL_fs_transition_size;
extern const uint8_t  kEmbeddedGL_fs_vfx[];
extern const size_t   kEmbeddedGL_fs_vfx_size;
extern const uint8_t  kEmbeddedGL_stretch_blt_fs[];
extern const size_t   kEmbeddedGL_stretch_blt_fs_size;
extern const uint8_t  kEmbeddedGL_affine_blt_fs[];
extern const size_t   kEmbeddedGL_affine_blt_fs_size;
extern const uint8_t  kEmbeddedGL_fs_postfx_vignette[];
extern const size_t   kEmbeddedGL_fs_postfx_vignette_size;
extern const uint8_t  kEmbeddedGL_fs_postfx_lut[];
extern const size_t   kEmbeddedGL_fs_postfx_lut_size;
extern const uint8_t  kEmbeddedGL_fs_postfx_blur[];
extern const size_t   kEmbeddedGL_fs_postfx_blur_size;
extern const uint8_t  kEmbeddedGL_fs_postfx_bloom[];
extern const size_t   kEmbeddedGL_fs_postfx_bloom_size;
extern const uint8_t  kEmbeddedGL_fs_postfx_lut3d[];
extern const size_t   kEmbeddedGL_fs_postfx_lut3d_size;

// ---------------------------------------------------------------------------
// Precompiled Metal MSL bytecodes (Metal backend)
// Compiled from shaders/glsl/*.sc via bgfx shaderc (--platform osx)
// ---------------------------------------------------------------------------
extern const uint8_t  kEmbeddedMetal_vs_sprite[];
extern const size_t   kEmbeddedMetal_vs_sprite_size;
extern const uint8_t  kEmbeddedMetal_vs_fullscreen[];
extern const size_t   kEmbeddedMetal_vs_fullscreen_size;
extern const uint8_t  kEmbeddedMetal_stretch_blt_vs[];
extern const size_t   kEmbeddedMetal_stretch_blt_vs_size;
extern const uint8_t  kEmbeddedMetal_affine_blt_vs[];
extern const size_t   kEmbeddedMetal_affine_blt_vs_size;
extern const uint8_t  kEmbeddedMetal_fs_texture[];
extern const size_t   kEmbeddedMetal_fs_texture_size;
extern const uint8_t  kEmbeddedMetal_fs_modulated_texture[];
extern const size_t   kEmbeddedMetal_fs_modulated_texture_size;
extern const uint8_t  kEmbeddedMetal_fs_blend[];
extern const size_t   kEmbeddedMetal_fs_blend_size;
extern const uint8_t  kEmbeddedMetal_fs_transition[];
extern const size_t   kEmbeddedMetal_fs_transition_size;
extern const uint8_t  kEmbeddedMetal_fs_vfx[];
extern const size_t   kEmbeddedMetal_fs_vfx_size;
extern const uint8_t  kEmbeddedMetal_stretch_blt_fs[];
extern const size_t   kEmbeddedMetal_stretch_blt_fs_size;
extern const uint8_t  kEmbeddedMetal_affine_blt_fs[];
extern const size_t   kEmbeddedMetal_affine_blt_fs_size;
// 035: Metal postfx arrays (shaderc osx).
extern const uint8_t  kEmbeddedMetal_fs_postfx_vignette[];
extern const size_t   kEmbeddedMetal_fs_postfx_vignette_size;
extern const uint8_t  kEmbeddedMetal_fs_postfx_lut[];
extern const size_t   kEmbeddedMetal_fs_postfx_lut_size;
extern const uint8_t  kEmbeddedMetal_fs_postfx_blur[];
extern const size_t   kEmbeddedMetal_fs_postfx_blur_size;
extern const uint8_t  kEmbeddedMetal_fs_postfx_bloom[];
extern const size_t   kEmbeddedMetal_fs_postfx_bloom_size;
extern const uint8_t  kEmbeddedMetal_fs_postfx_lut3d[];
extern const size_t   kEmbeddedMetal_fs_postfx_lut3d_size;

// -- Round-102 post-processing chain full-screen PS (DXBC, D3D11/D3D12) --------
// Compiled from shaders/dx11/fs_postfx_*.hlsl via fxc /T ps_4_0 /E main.
extern const uint8_t  kEmbeddedDXBC_fs_postfx_vignette[];
extern const size_t   kEmbeddedDXBC_fs_postfx_vignette_size;
extern const uint8_t  kEmbeddedDXBC_fs_postfx_lut[];
extern const size_t   kEmbeddedDXBC_fs_postfx_lut_size;
extern const uint8_t  kEmbeddedDXBC_fs_postfx_blur[];
extern const size_t   kEmbeddedDXBC_fs_postfx_blur_size;
extern const uint8_t  kEmbeddedDXBC_fs_postfx_bloom[];
extern const size_t   kEmbeddedDXBC_fs_postfx_bloom_size;
extern const uint8_t  kEmbeddedDXBC_fs_postfx_lut3d[];
extern const size_t   kEmbeddedDXBC_fs_postfx_lut3d_size;

} // namespace Caesura
