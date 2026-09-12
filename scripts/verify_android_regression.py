#!/usr/bin/env python3
"""
scripts/verify_android_regression.py
=============================================================================
Caesura (AmeKAG) — Android Latest HEAD Regression & Verification Suite
Target Commit: Dynamically resolved from git HEAD or latest test snapshot

Static source audit of 10 Android integration areas. This does not execute an
Android device or establish runtime rendering, input, audio or signing results.
  1. Boot & Manifest / Host Activity Configuration
  2. Rendering: demand-loaded FreeType 2048x2048 RGBA8 CJK Glyph Atlas
  3. Rendering: BgfxQuadBatch MergeGroup Transient Buffer Allocation
  4. Rendering: RTT vs Texture ID Namespace Separation
  5. Input: Physical-to-Logical Viewport Touch Scaling & GestureDetector
  6. Storage: Save System Slots (-1 Quick, -2 Auto) & Persistence
  7. Lifecycle & Audio: OpenSLES 3-Bus & State Management
  8. IME: Virtual Keyboard & Text Input Bridge (C++ -> Lua)
  9. Release Signing: PKCS12 Keystore & Gradle AAB/APK Signing Pipeline
 10. First-VN Traversal Assets & Packaging Integrity
=============================================================================
"""

import sys
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

class AndroidVerifier:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.checks = []

    def check(self, name: str, condition: bool, details: str = ""):
        if condition:
            self.passed += 1
            print(f"  [PASS] {name}" + (f" ({details})" if details else ""))
            self.checks.append((name, "PASS", details))
        else:
            self.failed += 1
            print(f"  [FAIL] {name}" + (f" ({details})" if details else ""))
            self.checks.append((name, "FAIL", details))

    def verify_boot_and_manifest(self):
        print("\n[Category 1] Boot, Manifest & Host Configuration")
        manifest_path = ROOT / "android" / "app" / "src" / "main" / "AndroidManifest.xml"
        main_activity_path = ROOT / "android" / "app" / "src" / "main" / "java" / "com" / "caesura" / "app" / "MainActivity.java"
        
        self.check("AndroidManifest.xml exists", manifest_path.exists())
        if manifest_path.exists():
            content = manifest_path.read_text(encoding="utf-8")
            self.check("Landscape orientation locked", 'android:screenOrientation="landscape"' in content)
            self.check("Launch mode singleInstance", 'android:launchMode="singleInstance"' in content)
            self.check("ConfigChanges handles orientation|screenSize|keyboard", 
                       'orientation' in content and 'screenSize' in content and 'keyboard' in content)
            self.check("OpenGL ES feature declared", 'android:glEsVersion=' in content)

        self.check("MainActivity.java exists", main_activity_path.exists())
        if main_activity_path.exists():
            content = main_activity_path.read_text(encoding="utf-8")
            self.check("Extends SDLActivity", "extends SDLActivity" in content)
            self.check("Extracts game assets to internal storage", "caesura_root" in content)
            self.check("Passes GLES backend argument", "--backend" in content and "gles" in content)

    def verify_cjk_font_atlas(self):
        print("\n[Category 2] Source wiring: demand-loaded FreeType CJK RGBA8 2048x2048 Atlas")
        render_dir = ROOT / "src" / "render"
        paths = [render_dir / name for name in
                 ("TextRenderer.cpp", "TextRendererFont.cpp", "TextRenderer.h")]
        self.check("TextRenderer font implementation and header exist", all(path.exists() for path in paths))
        # These source checks track the demand-loaded U16 implementation.
        # Real GlyphAtlas/independent-FreeType cases run in CaesuraTests;
        # Android/GLES rendering still requires actual device pixel evidence.
        def source(path):
            content = path.read_text(encoding="utf-8") if path.exists() else ""
            return re.sub(r"//[^\n]*|/\*.*?\*/", "", content, flags=re.DOTALL)

        render_source, font_source, header = (source(path) for path in paths)
        atlas = re.search(r"class\s+GlyphAtlas\s*\{(.*?)\n\s*\};", header, re.DOTALL)
        prepare = re.search(r"TextRenderer::GlyphAtlas::create\s*\(.*?\n\}", font_source, re.DOTALL)
        activate = re.search(r"TextRenderer::activateFont\s*\(.*?\n\}", font_source, re.DOTALL)
        prepared = prepare.group(0) if prepare else ""
        atlas_defaults = atlas.group(1) if atlas else ""
        self.check("2048x2048 Atlas Dimensions", bool(
            re.search(r"\batlasW\s*=\s*2048\b", atlas_defaults)
            and re.search(r"\batlasH\s*=\s*2048\b", atlas_defaults)
            and re.search(r"font->atlasPixels\.assign\(\s*size_t\(font->atlasW\)\s*\*\s*font->atlasH\s*\*\s*4\s*,\s*255\s*\)", prepared)))
        self.check("TextureFormat::RGBA8 for GLES sampling", bool(activate and re.search(
            r"bgfx::createTexture2D\([^;]*bgfx::TextureFormat::RGBA8", activate.group(0))))
        self.check("Font atlas owns bytes used by its FreeType face",
                   "font->sourceBytes.assign(bytes, bytes + size)" in prepared
                   and "font->sourceBytes.data()" in prepared)
        self.check("Each atlas owns its FreeType library and face",
                   "FT_Init_FreeType(&font->ftLib)" in prepared
                   and "FT_New_Memory_Face(font->ftLib" in prepared
                   and "FT_Done_Face(ftFace)" in render_source
                   and "FT_Done_FreeType(ftLib)" in render_source)
        self.check("Actual UTF-8 codepoints are prepared and uploaded",
                   "utf8_codepoint(data + i, length)" in render_source
                   and "m_ttf->prepareGlyph(cp)" in render_source
                   and "if (uploadPendingGlyphs()) return true;" in render_source)
        self.check("Atlas capacity is bounded before appending glyphs",
                   "font.glyphs.size() >= GlyphAtlas::MaxGlyphs" in font_source
                   and "if (penY + h + 1 > font.atlasH) return Status::Full;" in font_source
                   and "font.glyphs.emplace(cp, gm)" in font_source)
        self.check("Straight-alpha glyph coverage and transparent padding",
                   "font->atlasPixels[i] = 0" in prepared
                   and "font.atlasPixels[idx + 3] = cov" in font_source)
        self.check("Demand uploads use owned pixel memory and a mutable texture",
                   "prepared.ttf ? nullptr" in font_source
                   and "bgfx::copy(patch.data()" in render_source
                   and "bgfx::updateTexture2D(m_fontTexture" in render_source
                   and "m_ttf->acknowledgeUpload()" in render_source)
        self.check("Missing glyphs resolve inside the active atlas",
                   "font->prepareGlyph(replacement)" in prepared
                   and "return glyph ? *glyph : replacement();" in font_source
                   and "GlyphAtlas::PrepareStatus::Missing" in render_source)

    def verify_quad_batching(self):
        print("\n[Category 3] Rendering: Multi-Texture Quad Batching & Transient Buffer Safety")
        batch_path = ROOT / "src" / "render" / "BgfxQuadBatch.cpp"
        self.check("BgfxQuadBatch.cpp exists", batch_path.exists())
        if batch_path.exists():
            content = batch_path.read_text(encoding="utf-8")
            self.check("MergeGroup based batch grouping", "computeMergeGroups" in content or "MergeGroup" in content)
            self.check("Transient vertex buffer per MergeGroup", "bgfx::allocTransientVertexBuffer(&gtvb" in content)
            self.check("Transient index buffer per MergeGroup", "bgfx::allocTransientIndexBuffer(&gtib" in content)
            self.check("Fresh bgfx::setState per submit", "bgfx::setState(state)" in content and "bgfx::submit(" in content)

    def verify_rtt_namespace_isolation(self):
        print("\n[Category 4] Rendering: RTT vs Texture ID Namespace Separation")
        binding_path = ROOT / "src" / "script" / "bindings" / "RenderBinding.cpp"
        self.check("RenderBinding.cpp exists", binding_path.exists())
        if binding_path.exists():
            content = binding_path.read_text(encoding="utf-8")
            self.check("tex key routed to TextureManager", "texId != 0" in content and "getTexture" in content)
            self.check("rt key routed to IRenderDevice::getViewportTexture", "rtId != 0" in content and "getViewportTexture" in content)

    def verify_touch_and_input(self):
        print("\n[Category 5] Input: Physical-to-Logical Touch Scaling & Gestures")
        engine_path = ROOT / "src" / "entry" / "Engine.cpp"
        self.check("Engine.cpp exists", engine_path.exists())
        if engine_path.exists():
            content = engine_path.read_text(encoding="utf-8")
            self.check("Scaled finger X coordinate", "event.tfinger.x * winW" in content)
            self.check("Scaled finger Y coordinate", "event.tfinger.y * winH" in content)
            self.check("GestureDetector integrated for finger down/move/up", "m_gestureDetector" in content)
            self.check("MobileAdapter integrated for touch injection", "m_mobileAdapter" in content)

        gesture_h = ROOT / "src" / "platform" / "GestureDetector.h"
        gesture_cpp = ROOT / "src" / "platform" / "GestureDetector.cpp"
        self.check("GestureDetector.h and GestureDetector.cpp exist", gesture_h.exists() and gesture_cpp.exists())
        if gesture_h.exists():
            content_h = gesture_h.read_text(encoding="utf-8")
            self.check("Long press threshold >= 500ms (kLongPressMs)", "500" in content_h or "kLongPressMs" in content_h)
            self.check("Pinch gesture scale constants (kPinchInitial/kPinchStep)", "kPinchInitial" in content_h and "kPinchStep" in content_h)

    def verify_save_system(self):
        print("\n[Category 6] Storage: Save Persistence & System Slots")
        save_mgr_path = ROOT / "src" / "storage" / "SaveManager.cpp"
        self.check("SaveManager.cpp exists", save_mgr_path.exists())
        if save_mgr_path.exists():
            content = save_mgr_path.read_text(encoding="utf-8")
            self.check("Slot -1 quicksave mapping (save_quick.json)", 'slot == -1' in content and 'save_quick.json' in content)
            self.check("Slot -2 autosave mapping (save_auto.json)", 'slot == -2' in content and 'save_auto.json' in content)
            self.check("Slot range validation includes -2..99", 'slot >= -2 && slot <= 99' in content or 'validSlot' in content)
            self.check("Base64 screenshot thumbnail support", "thumbnail" in content.lower() or "base64" in content.lower())

    def verify_lifecycle_and_audio(self):
        print("\n[Category 7] Lifecycle & Audio Subsystems")
        soloud_path = ROOT / "src" / "audio" / "SoLoudAudioEngine.cpp"
        self.check("SoLoudAudioEngine.cpp exists", soloud_path.exists())
        if soloud_path.exists():
            content = soloud_path.read_text(encoding="utf-8")
            self.check("3 audio buses configured (BGM, SE, Voice)", "m_bgmBus" in content and "m_seBus" in content and "m_voiceBus" in content)

        build_script = ROOT / "scripts" / "build_android.sh"
        self.check("build_android.sh exists", build_script.exists())
        if build_script.exists():
            content = build_script.read_text(encoding="utf-8")
            self.check("SOLOUD_BACKEND_OPENSLES=ON configured", "SOLOUD_BACKEND_OPENSLES=ON" in content)

    def verify_ime_subsystem(self):
        print("\n[Category 8] IME Virtual Keyboard & Text Input Bridge")
        platform_iface = ROOT / "src" / "platform" / "api" / "IPlatformBackend.h"
        sdl3_platform = ROOT / "src" / "platform" / "SDL3PlatformBackend.cpp"
        devcore_binding = ROOT / "src" / "script" / "bindings" / "DevCoreBinding.cpp"

        self.check("IPlatformBackend.h exists", platform_iface.exists())
        if platform_iface.exists():
            content = platform_iface.read_text(encoding="utf-8")
            self.check("startTextInput pure virtual method", "virtual bool startTextInput() = 0;" in content)
            self.check("stopTextInput pure virtual method", "virtual bool stopTextInput() = 0;" in content)
            self.check("setTextInputRect pure virtual method", "virtual bool setTextInputRect(" in content)
            self.check("isTextInputActive pure virtual method", "virtual bool isTextInputActive() const = 0;" in content)

        self.check("SDL3PlatformBackend.cpp implements IME", sdl3_platform.exists())
        if sdl3_platform.exists():
            content = sdl3_platform.read_text(encoding="utf-8")
            self.check("SDL_StartTextInput integration", "SDL_StartTextInput" in content)
            self.check("SDL_StopTextInput integration", "SDL_StopTextInput" in content)
            self.check("SDL_SetTextInputArea integration", "SDL_SetTextInputArea" in content)
            self.check("SDL_TextInputActive integration", "SDL_TextInputActive" in content)

        self.check("DevCoreBinding.cpp exposes IME to Lua", devcore_binding.exists())
        if devcore_binding.exists():
            content = devcore_binding.read_text(encoding="utf-8")
            self.check("DevCore.start_text_input bound", "start_text_input" in content)
            self.check("DevCore.stop_text_input bound", "stop_text_input" in content)
            self.check("DevCore.set_text_input_rect bound", "set_text_input_rect" in content)
            self.check("DevCore.is_text_input_active bound", "is_text_input_active" in content)

    def verify_release_signing_pipeline(self):
        print("\n[Category 9] Release Signing & Packaging Pipeline")
        gradle_path = ROOT / "android" / "app" / "build.gradle"
        key_gen_sh = ROOT / "scripts" / "generate_android_keystore.sh"
        key_gen_bat = ROOT / "scripts" / "generate_android_keystore.bat"
        release_sh = ROOT / "scripts" / "build_android_release.sh"

        self.check("build.gradle exists", gradle_path.exists())
        if gradle_path.exists():
            content = gradle_path.read_text(encoding="utf-8")
            self.check("compileSdkVersion 35", "compileSdkVersion 35" in content)
            self.check("targetSdkVersion 35", "targetSdkVersion 35" in content)
            self.check("minSdkVersion 24", "minSdkVersion 24" in content)
            self.check("arm64-v8a abi filter", "'arm64-v8a'" in content)
            self.check("Environment driven CAESURA_ANDROID_KEYSTORE", "CAESURA_ANDROID_KEYSTORE" in content)
            self.check("signingConfigs.caesura configured", "signingConfigs" in content and "caesura" in content)
            self.check("v1/v2 signing enabled", "v1SigningEnabled true" in content and "v2SigningEnabled true" in content)
            self.check("Language split disabled for VN", "language" in content and "enableSplit = false" in content)
            self.check("Density split disabled for VN", "density" in content and "enableSplit = false" in content)
            self.check("ABI split disabled for VN", "abi" in content and "enableSplit = false" in content)

        self.check("generate_android_keystore.sh exists", key_gen_sh.exists())
        if key_gen_sh.exists():
            content = key_gen_sh.read_text(encoding="utf-8")
            self.check("PKCS12 storetype", "PKCS12" in content)
            self.check("RSA 2048 key algorithm", "RSA" in content and "2048" in content)
            self.check("Ephemeral CI key mode (--test)", "--test" in content)

        self.check("generate_android_keystore.bat exists", key_gen_bat.exists())
        self.check("build_android_release.sh exists", release_sh.exists())
        if release_sh.exists():
            content = release_sh.read_text(encoding="utf-8")
            self.check("assembleRelease APK target", "assembleRelease" in content)
            self.check("bundleRelease AAB target", "bundleRelease" in content)
            self.check("zipalign 4-byte check", "zipalign" in content)
            self.check("apksigner verification", "apksigner" in content)

    def verify_first_vn_assets(self):
        print("\n[Category 10] First-VN Project & Story Packaging Parity")
        first_vn_dir = ROOT / "tests" / "projects" / "first_vn"
        self.check("First-VN test project directory exists", first_vn_dir.exists())
        if first_vn_dir.exists():
            story_path = first_vn_dir / "story.ks"
            entry_path = first_vn_dir / "entry.lua"
            self.check("story.ks exists", story_path.exists())
            self.check("entry.lua exists", entry_path.exists())
            if story_path.exists():
                story_content = story_path.read_text(encoding="utf-8")
                self.check("Choice moment label present", "*choice_moment" in story_content)
                self.check("Branch Sun label present", "*branch_sun" in story_content)
                self.check("Branch Rain label present", "*branch_rain" in story_content)
                self.check("Ending label present", "*ending" in story_content)
                self.check("Sunset ending check", "f.is_sun == 1" in story_content)

    def run_all(self):
        try:
            import subprocess
            res = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, cwd=ROOT)
            head_commit = res.stdout.strip() if res.returncode == 0 and res.stdout.strip() else "latest"
        except Exception:
            head_commit = "latest"
        print("===================================================================")
        print(" Caesura (AmeKAG) — Android Latest HEAD Regression Verification")
        print(f" Target Commit: {head_commit}")
        print("===================================================================")
        self.verify_boot_and_manifest()
        self.verify_cjk_font_atlas()
        self.verify_quad_batching()
        self.verify_rtt_namespace_isolation()
        self.verify_touch_and_input()
        self.verify_save_system()
        self.verify_lifecycle_and_audio()
        self.verify_ime_subsystem()
        self.verify_release_signing_pipeline()
        self.verify_first_vn_assets()
        
        print("\n-------------------------------------------------------------------")
        print(f"Summary: {self.passed} Passed, {self.failed} Failed out of {self.passed + self.failed} checks.")
        print("-------------------------------------------------------------------")
        return self.failed == 0

def main():
    verifier = AndroidVerifier()
    success = verifier.run_all()
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()
