# Caesura engine module targets.
#
# Each subsystem is compiled once as a static library. API-only targets keep
# BackendRegistry dependencies at the interface layer so implementation
# targets remain acyclic.

add_library(CaesuraBuildOptions INTERFACE)
add_library(Caesura::BuildOptions ALIAS CaesuraBuildOptions)

include(${CMAKE_CURRENT_LIST_DIR}/CaesuraValidation.cmake)

# Engine version propagated from the root project() so every module (save
# envelopes, diagnostics UI) reads the released binary's version.
target_compile_definitions(CaesuraBuildOptions INTERFACE
    CAESURA_VERSION="${PROJECT_VERSION}"
    # Repo root for out-of-tree builds (WSL/CI build dirs are NOT on the
    # cwd chain): editor path confinement resolves templates/projects from
    # the source tree instead of guessing upward from the build dir.
    # u8 keeps char8_t semantics that std::filesystem::path accepts on MSVC
    # (Chinese repo paths must survive the ACP conversion).
    CAESURA_SOURCE_DIR=u8"${CMAKE_SOURCE_DIR}"
)

target_compile_features(CaesuraBuildOptions INTERFACE cxx_std_20)
target_include_directories(CaesuraBuildOptions INTERFACE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external
    ${CMAKE_SOURCE_DIR}/external/soloud/include
    ${CMAKE_SOURCE_DIR}/external/bgfx/bx/include
    ${CMAKE_SOURCE_DIR}/external/bgfx/bimg/include
    ${CMAKE_SOURCE_DIR}/external/bgfx/bgfx/include
    ${CMAKE_SOURCE_DIR}/external/bgfx/bgfx/3rdparty
    ${CMAKE_SOURCE_DIR}/external/stb
    ${CMAKE_SOURCE_DIR}/external/bgfx/bimg/3rdparty/stb
    ${CMAKE_SOURCE_DIR}/external/bgfx/bgfx/3rdparty/stb
    ${CMAKE_SOURCE_DIR}/external/pl_mpeg
    ${CMAKE_SOURCE_DIR}/external/freetype/include
    ${CMAKE_SOURCE_DIR}/external/zstd/lib
    ${CMAKE_SOURCE_DIR}/external/cpp-httplib
    ${CMAKE_SOURCE_DIR}/external/json
    ${CMAKE_SOURCE_DIR}/external/lua
    ${CMAKE_SOURCE_DIR}/external/ed25519
)
target_compile_definitions(CaesuraBuildOptions INTERFACE
    # Track M A2: Android's SDLActivity glue calls the app's `SDL_main`
    # entry (getMainFunction()=SDL_main); defining SDL_MAIN_HANDLED there
    # would keep a plain `main` and the JNI glue could never start the app.
    # Desktop platforms keep SDL_MAIN_HANDLED (their wrappers are ours).
    $<$<NOT:$<PLATFORM_ID:Android>>:SDL_MAIN_HANDLED>
    $<$<CONFIG:Debug>:BX_CONFIG_DEBUG=1>
    $<$<NOT:$<CONFIG:Debug>>:BX_CONFIG_DEBUG=0>
)

if(CAESURA_DEBUG)
    target_compile_definitions(CaesuraBuildOptions INTERFACE CAESURA_DEBUG)
endif()

if(MSVC)
    target_compile_options(CaesuraBuildOptions INTERFACE
        /W3 /wd4100 /wd4189 /wd4244
        /Zc:__cplusplus /Zc:preprocessor /utf-8 /FS
    )
    target_compile_definitions(CaesuraBuildOptions INTERFACE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        __STDC_LIMITS_MACROS
        __STDC_FORMAT_MACROS
        __STDC_CONSTANT_MACROS
    )
endif()

add_library(CaesuraSystemDependencies INTERFACE)
add_library(Caesura::SystemDependencies ALIAS CaesuraSystemDependencies)

if(WIN32)
    target_link_libraries(CaesuraSystemDependencies INTERFACE
        bcrypt winmm ws2_32 psapi ole32 d3d11 dxgi d3dcompiler
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # iOS (Track I): Cocoa is macOS-only (no such framework on iOS) - link
    # the iOS equivalents instead. Metal/QuartzCore cover bgfx Metal;
    # Foundation/UIKit for the app layer; AudioToolbox for SoLoud CoreAudio;
    # OpenSSL stays out until a real iOS build supplies it (archive crypto).
    find_library(METAL_LIBRARY Metal REQUIRED)
    find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
    find_library(QUARTZCORE_LIBRARY QuartzCore REQUIRED)
    find_library(UIKIT_LIBRARY UIKit REQUIRED)
    find_library(AUDIOTOOLBOX_LIBRARY AudioToolbox REQUIRED)
    # Archive crypto needs OpenSSL on every platform - the iOS probe builds
    # an iOS slice and passes OPENSSL_ROOT_DIR (same pattern as SDL3_DIR).
    # FindOpenSSL on the mac runner prefers Homebrew pkg-config paths and
    # ignores OPENSSL_ROOT_DIR, so resolve the explicit install directly.
    # Direct absolute paths: find_package(FindOpenSSL) prefers Homebrew on
    # the mac runner; find_library behaved inconsistently with -D cache vars
    # here, so the probe contract is explicit file paths under the root.
    set(IOS_OPENSSL_SSL "${OPENSSL_ROOT_DIR}/lib/libssl.a")
    set(IOS_OPENSSL_CRYPTO "${OPENSSL_ROOT_DIR}/lib/libcrypto.a")
    if(NOT EXISTS "${IOS_OPENSSL_SSL}" OR NOT EXISTS "${IOS_OPENSSL_CRYPTO}")
        message(FATAL_ERROR "iOS OpenSSL slice missing under OPENSSL_ROOT_DIR: ${OPENSSL_ROOT_DIR} (need lib/libssl.a + lib/libcrypto.a)")
    endif()
    target_include_directories(CaesuraSystemDependencies INTERFACE
        "${OPENSSL_ROOT_DIR}/include")
    target_link_libraries(CaesuraSystemDependencies INTERFACE
        ${METAL_LIBRARY}
        ${FOUNDATION_LIBRARY}
        ${QUARTZCORE_LIBRARY}
        ${UIKIT_LIBRARY}
        ${AUDIOTOOLBOX_LIBRARY}
        pthread
        "${IOS_OPENSSL_SSL}"
        "${IOS_OPENSSL_CRYPTO}"
    )
elseif(APPLE)
    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    find_library(IOKIT_LIBRARY IOKit REQUIRED)
    find_library(METAL_LIBRARY Metal REQUIRED)
    find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
    find_library(QUARTZCORE_LIBRARY QuartzCore REQUIRED)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(CaesuraSystemDependencies INTERFACE
        ${COCOA_LIBRARY}
        ${IOKIT_LIBRARY}
        ${METAL_LIBRARY}
        ${QUARTZCORE_LIBRARY}
        ${FOUNDATION_LIBRARY}
        pthread
        OpenSSL::SSL
        OpenSSL::Crypto
    )


elseif(ANDROID)
    # Track M: Android (NDK) is POSIX but desktop-only deps (X11) do not
    # exist. Same OpenSSL slice contract as the iOS branch: the probe
    # builds android-arm64 libssl.a/libcrypto.a under OPENSSL_ROOT_DIR and
    # passes it explicitly — FindOpenSSL is unreliable cross-compiling
    # (pkg-config paths win on host).
    find_package(Threads REQUIRED)
    set(ANDROID_OPENSSL_SSL "${OPENSSL_ROOT_DIR}/lib/libssl.a")
    set(ANDROID_OPENSSL_CRYPTO "${OPENSSL_ROOT_DIR}/lib/libcrypto.a")
    if(NOT EXISTS "${ANDROID_OPENSSL_SSL}" OR NOT EXISTS "${ANDROID_OPENSSL_CRYPTO}")
        message(FATAL_ERROR "Android OpenSSL slice missing under OPENSSL_ROOT_DIR: ${OPENSSL_ROOT_DIR} (need lib/libssl.a + lib/libcrypto.a)")
    endif()
    target_include_directories(CaesuraSystemDependencies INTERFACE
        "${OPENSSL_ROOT_DIR}/include")
    target_link_libraries(CaesuraSystemDependencies INTERFACE
        Threads::Threads
        log android
        EGL GLESv2
        "${ANDROID_OPENSSL_SSL}"
        "${ANDROID_OPENSSL_CRYPTO}"
    )
elseif(UNIX)
    find_package(Threads REQUIRED)
    find_package(X11 REQUIRED)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(CaesuraSystemDependencies INTERFACE
        Threads::Threads
        dl
        X11::X11
        OpenSSL::SSL
        OpenSSL::Crypto
    )
endif()

set(CAESURA_API_INCLUDE_ROOT "${CMAKE_BINARY_DIR}/caesura_api_include")

function(caesura_add_api module)
    string(TOLOWER "${module}" module_dir)
    set(module_api_dir "${CMAKE_SOURCE_DIR}/src/${module_dir}/api")
    set(module_api_view "${CAESURA_API_INCLUDE_ROOT}/${module_dir}/api")
    file(MAKE_DIRECTORY "${module_api_view}")
    file(GLOB module_api_headers CONFIGURE_DEPENDS "${module_api_dir}/*.h")
    foreach(header IN LISTS module_api_headers)
        get_filename_component(header_name "${header}" NAME)
        configure_file("${header}" "${module_api_view}/${header_name}" COPYONLY)
    endforeach()

    add_library(Caesura${module}Api INTERFACE)
    add_library(Caesura::${module}Api ALIAS Caesura${module}Api)
    target_compile_features(Caesura${module}Api INTERFACE cxx_std_20)
    target_include_directories(Caesura${module}Api INTERFACE
        $<BUILD_INTERFACE:${CAESURA_API_INCLUDE_ROOT}>
    )
    set_target_properties(Caesura${module}Api PROPERTIES FOLDER "Caesura/API")
endfunction()

foreach(module IN ITEMS
        Archive Audio Debug Di Input Job Live2D MiniGame Platform Render
        Resource Rpc Script Steam Storage)
    caesura_add_api(${module})
endforeach()

# Public interfaces that expose third-party types propagate those usage
# requirements to consumers.
target_link_libraries(CaesuraInputApi INTERFACE SDL3::SDL3)
target_link_libraries(CaesuraRpcApi INTERFACE CaesuraArchiveApi)
target_include_directories(CaesuraStorageApi INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/external/json>
)

function(caesura_add_module module)
    add_library(Caesura${module} STATIC ${ARGN})
    add_library(Caesura::${module} ALIAS Caesura${module})
    target_link_libraries(Caesura${module}
        PUBLIC Caesura${module}Api
        PRIVATE CaesuraBuildOptions
    )
    string(TOLOWER "${module}" module_output_name)
    set_target_properties(Caesura${module} PROPERTIES
        FOLDER "Caesura/Modules"
        OUTPUT_NAME "caesura_${module_output_name}"
    )
endfunction()

caesura_add_module(Archive
    src/archive/CryptoEngine.cpp
    src/archive/CARCReader.cpp
    src/archive/CARCWriter.cpp
    src/archive/CarcAssetProvider.cpp
    src/archive/CRLManager.cpp
    src/archive/DeltaCARC.cpp
)

caesura_add_module(Audio
    src/audio/SoLoudAudioEngine.cpp
    src/audio/AudioRestore.cpp
    src/audio/NullAudioBackend.cpp
)

caesura_add_module(Debug
    src/debug/DebugManager.cpp
    src/debug/HotReload.cpp
    src/debug/DebugProtocol.cpp
)

caesura_add_module(Di
    src/di/BackendRegistry.cpp
    src/di/TextureBudget.cpp
    src/di/SandboxQuota.cpp
    src/di/thread/ThreadAssert.cpp
)

caesura_add_module(Input
    src/input/InputRouter.cpp
)

caesura_add_module(Job
    src/job/JobSystem.cpp
)

caesura_add_module(Live2D
    src/live2d/NullAnimationBackend.cpp
    src/live2d/PathConfinement.cpp
)

caesura_add_module(MiniGame
    src/minigame/NullMiniGameBackend.cpp
    src/minigame/BgfxMiniGameBackend.cpp
    src/minigame/EmbeddedShaders_MiniGame.cpp
    src/minigame/EmbeddedShaders_MiniGame_GLSL.cpp
    src/minigame/EmbeddedShaders_MiniGame_Metal.cpp
    src/minigame/MiniGeometry.cpp
    src/minigame/MiniCollision.cpp
)

caesura_add_module(Platform
    src/platform/NullPlatformBackend.cpp
    src/platform/SDL3PlatformBackend.cpp
    src/platform/SDL3DisplayService.cpp
    src/platform/MobileAdapter.cpp
    src/platform/GestureDetector.cpp
)

caesura_add_module(Render
    src/render/NullRenderDevice.cpp
    src/render/NullMeshRenderer.cpp
    src/render/SmaMeshRenderer.cpp
    src/render/BgfxRenderDevice.cpp
    src/render/BgfxDeviceCore.cpp
    src/render/BgfxDraw_Batch.cpp
    src/render/BgfxDraw_Blit.cpp
    src/render/BgfxDraw_Effects.cpp
    src/render/BgfxQuadBatch.cpp
    src/render/ColorFilterMath.cpp
    src/render/BgfxShaderManager.cpp
    src/render/BgfxDebugCallback.cpp
    src/render/ScreenshotQueue.cpp
    src/render/RTTManager.cpp
    src/render/EmbeddedShaders.cpp
    src/render/EmbeddedShaders_GL.cpp
    src/render/EmbeddedShaders_Metal.cpp
    src/render/EmbeddedShaders_S5.cpp
    src/render/TextRenderer.cpp
    src/render/TextRendererFont.cpp
    src/render/ParticleSystem.cpp
    src/render/GpuMonitor.cpp
    src/render/VideoPlayer.cpp
    src/render/TextureManager.cpp
    src/render/LayerManager.cpp
    src/render/ShaderCache.cpp
    src/render/stb_impl.cpp
)

caesura_add_module(Resource
    src/resource/XP3Archive.cpp
    src/resource/DirAssetProvider.cpp
    src/resource/ProviderChain.cpp
    src/resource/AssetManager.cpp
    src/resource/ImageDecoder.cpp
    src/resource/AsyncLoader.cpp
)

caesura_add_module(Rpc
    src/rpc/RpcServer.cpp
    src/rpc/EditorServer.cpp
    src/rpc/services/ProjectService.cpp
    src/rpc/services/PackagingService.cpp
    src/rpc/services/AssetService.cpp
)

caesura_add_module(Script
    src/script/vm/LuaManager.cpp
    src/script/state/GameState.cpp
    src/script/bindings/KAGBinding.cpp
    src/script/bindings/RenderBinding.cpp
    src/script/bindings/VFXBinding.cpp
    src/script/bindings/DevCoreBinding.cpp
    src/script/bindings/DebugBinding.cpp
    src/script/bindings/MiniGameBinding.cpp
    src/script/bindings/SmaBinding.cpp
    src/script/bindings/SteamBinding.cpp
    src/script/bindings/SaveBinding.cpp
    src/script/bindings/RestoreBinding.cpp
    src/script/bindings/AudioRestoreBinding.cpp
    src/script/bindings/FontRestoreBinding.cpp
    src/script/bindings/TransientRestoreBinding.cpp
    src/script/bindings/AIBinding.cpp
    src/script/bindings/EngineBinding.cpp
)

caesura_add_module(Steam
    src/steam/SteamBackend.cpp
)

caesura_add_module(Storage
    src/storage/SaveManager.cpp
    src/storage/ISaveProvider.cpp
    src/storage/CloudSaveProvider.cpp
    src/storage/HttpCloudSaveProvider.cpp
)

# BackendRegistry only knows API targets. Implementations may depend on the
# registry without creating implementation-level cycles.
target_link_libraries(CaesuraDi
    PUBLIC
        CaesuraArchiveApi
        CaesuraAudioApi
        CaesuraDebugApi
        CaesuraInputApi
        CaesuraJobApi
        CaesuraLive2DApi
        CaesuraMiniGameApi
        CaesuraPlatformApi
        CaesuraRenderApi
        CaesuraResourceApi
        CaesuraScriptApi
        CaesuraSteamApi
        CaesuraStorageApi
    PRIVATE
        lua
)

target_link_libraries(CaesuraArchive PRIVATE
    CaesuraResourceApi libzstd_static ed25519 CaesuraSystemDependencies
)
target_link_libraries(CaesuraAudio PRIVATE CaesuraDi soloud)
target_link_libraries(CaesuraDebug PRIVATE lua bgfx SDL3::SDL3)
target_link_libraries(CaesuraInput PRIVATE SDL3::SDL3)
target_link_libraries(CaesuraJob PRIVATE CaesuraDi CaesuraSystemDependencies)
target_link_libraries(CaesuraLive2D PRIVATE
    CaesuraDi CaesuraRenderApi bgfx bx
)
target_link_libraries(CaesuraMiniGame PRIVATE
    CaesuraDi CaesuraInputApi CaesuraRenderApi bgfx bx lua
)
target_link_libraries(CaesuraPlatform PRIVATE SDL3::SDL3 lua)
target_link_libraries(CaesuraRender PRIVATE
    CaesuraDi CaesuraDebug CaesuraJobApi
    bgfx bimg bx freetype CaesuraSystemDependencies
)
if(CAESURA_RENDER_TEST_FAULTS)
    # Only implementation code depends on this flag; public layouts stay identical.
    target_compile_definitions(CaesuraRender PRIVATE CAESURA_RENDER_TEST_FAULTS=1)
endif()
target_link_libraries(CaesuraResource PRIVATE
    CaesuraDi CaesuraJobApi SDL3::SDL3 bimg bx
)
target_include_directories(CaesuraResource PRIVATE
    ${CMAKE_SOURCE_DIR}/external/bgfx/bimg/3rdparty/tinyexr/deps/miniz
)
target_link_libraries(CaesuraScript PRIVATE
    CaesuraDi CaesuraInputApi CaesuraRenderApi CaesuraStorageApi
    CaesuraAudioApi CaesuraDebugApi CaesuraMiniGameApi
    CaesuraPlatformApi CaesuraResourceApi CaesuraSteamApi lua
)
target_link_libraries(CaesuraSteam PRIVATE CaesuraSystemDependencies)
target_link_libraries(CaesuraStorage PRIVATE
    CaesuraDi CaesuraArchiveApi CaesuraSteamApi bgfx
)

add_library(CaesuraEntry STATIC
    src/entry/Engine.cpp
    src/entry/Engine_Assets.cpp
    src/entry/Engine_Backends.cpp
    src/entry/Engine_Gpu.cpp
    src/entry/Engine_LuaRegistry.cpp
    src/entry/StartupScripts.cpp
    src/entry/StartupValidation.cpp
    src/entry/ErrorUI.cpp
)
add_library(Caesura::Entry ALIAS CaesuraEntry)
target_link_libraries(CaesuraEntry
    PUBLIC CaesuraBuildOptions
    PRIVATE
        CaesuraArchive
        CaesuraAudio
        CaesuraDebug
        CaesuraDi
        CaesuraInput
        CaesuraJob
        CaesuraLive2D
        CaesuraMiniGame
        CaesuraPlatform
        CaesuraRender
        CaesuraResource
        CaesuraScript
        CaesuraSteam
        CaesuraStorage
        SDL3::SDL3
        bgfx
        lua
        CaesuraSystemDependencies
)
set_target_properties(CaesuraEntry PROPERTIES
    FOLDER "Caesura/Modules"
    OUTPUT_NAME "caesura_entry"
)
# §15 crash diagnostics: the entry error screen shows the same version as the
# save envelopes. (Engine.cpp must not include ../storage/SaveManager.h --
# guarded by test_source_encoding.cpp.) CAESURA_VERSION is defined once on
# the CaesuraBuildOptions interface target (top of this file) and inherited
# here -- single source of truth is the root project().

# Convenience target for the application and tests. It has no compiled
# sources; consumers receive the complete static engine graph.
add_library(CaesuraEngine INTERFACE)
add_library(Caesura::Engine ALIAS CaesuraEngine)
target_link_libraries(CaesuraEngine INTERFACE
    CaesuraBuildOptions
    CaesuraSystemDependencies
    CaesuraEntry
    CaesuraArchive
    CaesuraAudio
    CaesuraDebug
    CaesuraDi
    CaesuraInput
    CaesuraJob
    CaesuraLive2D
    CaesuraMiniGame
    CaesuraPlatform
    CaesuraRender
    CaesuraResource
    CaesuraScript
    CaesuraSteam
    CaesuraStorage
)
set_target_properties(CaesuraEngine PROPERTIES FOLDER "Caesura")
