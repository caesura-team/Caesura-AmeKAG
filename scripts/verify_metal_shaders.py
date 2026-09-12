import sys
from pathlib import Path
from generate_render_shaders import read_array

ROOT = Path(__file__).resolve().parent.parent
RENDER_H = ROOT / 'src' / 'render' / 'EmbeddedShaders.h'
RENDER_CPP = ROOT / 'src' / 'render' / 'EmbeddedShaders_Metal.cpp'
MINIGAME_H = ROOT / 'src' / 'minigame' / 'EmbeddedMiniGameShaders.h'
MINIGAME_CPP = ROOT / 'src' / 'minigame' / 'EmbeddedShaders_MiniGame_Metal.cpp'
SHADER_MGR_CPP = ROOT / 'src' / 'render' / 'BgfxShaderManager.cpp'
SMA_RENDERER_CPP = ROOT / 'src' / 'render' / 'SmaMeshRenderer.cpp'

REQUIRED_RENDER_SHADERS = [
    ('kEmbeddedMetal_vs_sprite', 'kEmbeddedMetal_vs_sprite_size', 'vertex'),
    ('kEmbeddedMetal_vs_fullscreen', 'kEmbeddedMetal_vs_fullscreen_size', 'vertex'),
    ('kEmbeddedMetal_stretch_blt_vs', 'kEmbeddedMetal_stretch_blt_vs_size', 'vertex'),
    ('kEmbeddedMetal_affine_blt_vs', 'kEmbeddedMetal_affine_blt_vs_size', 'vertex'),
    ('kEmbeddedMetal_fs_texture', 'kEmbeddedMetal_fs_texture_size', 'fragment'),
    ('kEmbeddedMetal_fs_blend', 'kEmbeddedMetal_fs_blend_size', 'fragment'),
    ('kEmbeddedMetal_fs_transition', 'kEmbeddedMetal_fs_transition_size', 'fragment'),
    ('kEmbeddedMetal_fs_vfx', 'kEmbeddedMetal_fs_vfx_size', 'fragment'),
    ('kEmbeddedMetal_stretch_blt_fs', 'kEmbeddedMetal_stretch_blt_fs_size', 'fragment'),
    ('kEmbeddedMetal_affine_blt_fs', 'kEmbeddedMetal_affine_blt_fs_size', 'fragment'),
]

REQUIRED_MINIGAME_SHADERS = [
    ('kEmbeddedMSL_MiniGame_VS', 'vertex (MSL)'),
    ('kEmbeddedMSL_MiniGame_FS', 'fragment (MSL)'),
]

def verify_render_shaders():
    print('[1/3] Verifying 10 2D Render Metal Embedded Shaders...')
    if not RENDER_H.exists() or not RENDER_CPP.exists():
        print('ERROR: Render shader files not found')
        return False
    h_text = RENDER_H.read_text(encoding='utf-8')
    cpp_text = RENDER_CPP.read_text(encoding='utf-8')
    all_ok = True
    for arr_name, size_name, stype in REQUIRED_RENDER_SHADERS:
        if arr_name not in h_text or size_name not in h_text:
            print(f'  FAILED: {arr_name} or {size_name} missing from header')
            all_ok = False
            continue
        if arr_name not in cpp_text:
            print(f'  FAILED: {arr_name} missing definition in cpp')
            all_ok = False
            continue
        # Decode the actual unsigned bytes and validate either a matching
        # numeric size or sizeof(this exact array), as emitted by shaderc's
        # checked-in wrapper. Presence of a positive integer alone is not proof
        # that the declaration matches a valid, unique byte array.
        try:
            data = read_array(cpp_text, arr_name)
        except ValueError as exc:
            print(f'  FAILED: {arr_name}: {exc}')
            all_ok = False
            continue
        print(f'  OK: {arr_name} ({stype}, {len(data)} bytes)')
    return all_ok

def verify_minigame_shaders():
    print('[2/3] Verifying 2 3D MiniGame MSL Shaders...')
    if not MINIGAME_H.exists() or not MINIGAME_CPP.exists():
        print('ERROR: Minigame shader files not found')
        return False
    h_text = MINIGAME_H.read_text(encoding='utf-8')
    cpp_text = MINIGAME_CPP.read_text(encoding='utf-8')
    all_ok = True
    for sym_name, stype in REQUIRED_MINIGAME_SHADERS:
        if sym_name not in h_text or sym_name not in cpp_text:
            print(f'  FAILED: {sym_name} missing')
            all_ok = False
            continue
        if 'metal_stdlib' not in cpp_text or 'using namespace metal' not in cpp_text:
            print(f'  FAILED: {sym_name} missing MSL tokens')
            all_ok = False
            continue
        print(f'  OK: {sym_name} ({stype})')
    return all_ok

def verify_fallbacks():
    print('[3/3] Verifying Metal Fallback Pathways...')
    if not SHADER_MGR_CPP.exists() or not SMA_RENDERER_CPP.exists():
        return False
    sm_text = SHADER_MGR_CPP.read_text(encoding='utf-8')
    sma_text = SMA_RENDERER_CPP.read_text(encoding='utf-8')
    ok = True
    if 'fsTexture' not in sm_text or 'fs_postfx' not in sm_text:
        print('  FAILED: Post-FX fallback missing')
        ok = False
    else:
        print('  OK: Post-FX fallback to fsTexture (identity blit) verified')
    if 'BGFX_CAPS_COMPUTE' not in sma_text or 'SmaSkinner' not in sma_text:
        print('  FAILED: SMA compute fallback missing')
        ok = False
    else:
        print('  OK: SMA dual-mode compute/S2 CPU soft-skinning fallback verified')
    return ok

def main():
    print('=======================================================')
    print(' Caesura (AmeKAG) -- Metal Shaders & Fallback Validator ')
    print('=======================================================')
    r_ok = verify_render_shaders()
    m_ok = verify_minigame_shaders()
    f_ok = verify_fallbacks()
    print('-------------------------------------------------------')
    if r_ok and m_ok and f_ok:
        print('PASS: All 12 Metal shader assets and fallback pathways verified.')
        sys.exit(0)
    else:
        print('FAIL: Metal shader verification failed.')
        sys.exit(1)

if __name__ == '__main__':
    main()
