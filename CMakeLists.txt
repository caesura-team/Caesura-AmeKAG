cmake_minimum_required(VERSION 3.25)
project(CaesuraAmeKAG VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CAESURA_REQUIRE_TEST_PREREQUISITES "Fail configure when validation prerequisites are missing" OFF)
option(CAESURA_RENDER_TEST_FAULTS "Enable bounded per-instance shader input faults for render contract probes" OFF)

# Force modern CMake policies (FreeType sets 3.0...3.5 which can cause issues)
cmake_policy(SET CMP0053 NEW)  # Variable expansion in strings
cmake_policy(SET CMP0054 NEW)  # if() argument handling
cmake_policy(SET CMP0057 NEW)  # if() IN_LIST operator
cmake_policy(SET CMP0063 NEW)  # Honor visibility properties
cmake_policy(SET CMP0077 NEW)  # option() honors normal variables
# -- FreeType 2 (font rendering, spec [2.4]) ---------------------------------
# Disable optional deps: bzip2, PNG, HarfBuzz, Brotli
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/external/freetype ${CMAKE_BINARY_DIR}/freetype)

# -- zstd (CARC decompression, spec [9.4]) -----------------------------------
# Static lib only, no programs/tests
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/external/zstd/build/cmake ${CMAKE_BINARY_DIR}/zstd)

# Dependencies
# The repository ships a Windows SDL3 package. Prefer it when callers have not
# selected another package so a fresh configure does not depend on user caches.
if(WIN32 AND NOT SDL3_DIR AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(_CAESURA_BUNDLED_SDL3_DIR
        "${CMAKE_SOURCE_DIR}/external/SDL3/SDL3-3.2.0/cmake")
    if(EXISTS "${_CAESURA_BUNDLED_SDL3_DIR}/SDL3Config.cmake")
        set(SDL3_DIR "${_CAESURA_BUNDLED_SDL3_DIR}" CACHE PATH
            "Directory containing SDL3Config.cmake" FORCE)
        message(STATUS "SDL3: bundled (external/SDL3/SDL3-3.2.0)")
    endif()
    unset(_CAESURA_BUNDLED_SDL3_DIR)
endif()
find_package(SDL3 CONFIG REQUIRED)

# Normalize the SDL3 target name: package managers (vcpkg) may expose
# SDL3::SDL3, SDL3::SDL3-shared, SDL3::SDL3-static, or a bare SDL3 target.
# When none is defined, fall back to an interface shim so the rest of the
# build can always reference SDL3::SDL3.
if(NOT TARGET SDL3::SDL3)
    if(TARGET SDL3::SDL3-shared)
        add_library(SDL3::SDL3 ALIAS SDL3::SDL3-shared)
    elseif(TARGET SDL3::SDL3-static)
        add_library(SDL3::SDL3 ALIAS SDL3::SDL3-static)
    elseif(TARGET SDL3)
        add_library(SDL3::SDL3 ALIAS SDL3)
    else()
        add_library(SDL3::SDL3 INTERFACE IMPORTED)
        find_path(SDL3_INCLUDE_DIR SDL3/SDL.h)
        if(SDL3_INCLUDE_DIR)
            set_target_properties(SDL3::SDL3 PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${SDL3_INCLUDE_DIR}")
        endif()
        set_target_properties(SDL3::SDL3 PROPERTIES
            INTERFACE_LINK_LIBRARIES "SDL3")
    endif()
endif()

# -- FFmpeg (hardware-accelerated video decoding, ON by default) -----------
option(CAESURA_ENABLE_FFMPEG "Enable FFmpeg video decoding" ON)
if(CAESURA_ENABLE_FFMPEG)
    # Method 1: bundled in external/ffmpeg (Windows prebuilt)
    if(EXISTS "${CMAKE_SOURCE_DIR}/external/ffmpeg/include")
        set(FFMPEG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/external/ffmpeg/include")
        set(FFMPEG_LIBRARY_DIR "${CMAKE_SOURCE_DIR}/external/ffmpeg/lib")
        set(FFMPEG_BIN_DIR "${CMAKE_SOURCE_DIR}/external/ffmpeg/bin")
        find_library(FFMPEG_avcodec_LIBRARY  avcodec  PATHS ${FFMPEG_LIBRARY_DIR} NO_DEFAULT_PATH)
        find_library(FFMPEG_avformat_LIBRARY avformat PATHS ${FFMPEG_LIBRARY_DIR} NO_DEFAULT_PATH)
        find_library(FFMPEG_swscale_LIBRARY  swscale  PATHS ${FFMPEG_LIBRARY_DIR} NO_DEFAULT_PATH)
        find_library(FFMPEG_avutil_LIBRARY   avutil   PATHS ${FFMPEG_LIBRARY_DIR} NO_DEFAULT_PATH)
        find_library(FFMPEG_swresample_LIBRARY swresample PATHS ${FFMPEG_LIBRARY_DIR} NO_DEFAULT_PATH)
        if(FFMPEG_avcodec_LIBRARY AND FFMPEG_avformat_LIBRARY AND FFMPEG_swscale_LIBRARY AND FFMPEG_avutil_LIBRARY AND FFMPEG_swresample_LIBRARY)
            set(FFMPEG_FOUND TRUE)
            set(FFMPEG_LIBRARIES ${FFMPEG_avcodec_LIBRARY} ${FFMPEG_avformat_LIBRARY} ${FFMPEG_swscale_LIBRARY} ${FFMPEG_avutil_LIBRARY} ${FFMPEG_swresample_LIBRARY})
            message(STATUS "FFmpeg: bundled (external/ffmpeg)")
        endif()
    endif()

    # Method 2: vcpkg
    if(NOT FFMPEG_FOUND AND DEFINED VCPKG_INSTALLED_DIR)
        find_path(FFMPEG_INCLUDE_DIRS libavcodec/avcodec.h PATHS ${VCPKG_INSTALLED_DIR}/include)
        find_library(FFMPEG_avcodec_LIBRARY  avcodec  PATHS ${VCPKG_INSTALLED_DIR}/lib)
        find_library(FFMPEG_avformat_LIBRARY avformat PATHS ${VCPKG_INSTALLED_DIR}/lib)
        find_library(FFMPEG_swscale_LIBRARY  swscale  PATHS ${VCPKG_INSTALLED_DIR}/lib)
        find_library(FFMPEG_avutil_LIBRARY   avutil   PATHS ${VCPKG_INSTALLED_DIR}/lib)
        find_library(FFMPEG_swresample_LIBRARY swresample PATHS ${VCPKG_INSTALLED_DIR}/lib)
        if(FFMPEG_avcodec_LIBRARY AND FFMPEG_avformat_LIBRARY AND FFMPEG_swscale_LIBRARY AND FFMPEG_avutil_LIBRARY AND FFMPEG_swresample_LIBRARY)
            set(FFMPEG_FOUND TRUE)
            set(FFMPEG_LIBRARIES ${FFMPEG_avcodec_LIBRARY} ${FFMPEG_avformat_LIBRARY} ${FFMPEG_swscale_LIBRARY} ${FFMPEG_avutil_LIBRARY} ${FFMPEG_swresample_LIBRARY})
            message(STATUS "FFmpeg: vcpkg")
        endif()
    endif()

    # Method 3: pkg-config (Linux/macOS)
    if(NOT FFMPEG_FOUND)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(FFMPEG QUIET libavcodec libavformat libswscale libavutil libswresample)
        endif()
    endif()

    if(FFMPEG_FOUND)
        set(CAESURA_FFMPEG_FOUND TRUE)
        set(CAESURA_FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIRS})
        set(CAESURA_FFMPEG_LIBRARIES ${FFMPEG_LIBRARIES})
        message(STATUS "FFmpeg video decoding: ENABLED")
    else()
        message(STATUS "FFmpeg not found -- pl_mpeg fallback only")
        message(STATUS "  To enable: place FFmpeg dev build in external/ffmpeg/")
        message(STATUS "  Download: https://github.com/BtbN/FFmpeg-Builds/releases")
    endif()
endif()

# SoLoud backend config -- disable SDL2 (we use SDL3), enable WINMM+WASAPI on Windows
set(SOLOUD_BACKEND_SDL2 OFF CACHE BOOL "" FORCE)
if(WIN32)
    set(SOLOUD_BACKEND_WINMM ON CACHE BOOL "" FORCE)
    set(SOLOUD_BACKEND_WASAPI ON CACHE BOOL "" FORCE)
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
    # Linux: without a real backend SoLoud's AUTO picks nothing and init
    # fails with error 7 (silent break -- CI only caught it via audio tests
    # tolerating init failure, and headless WSL needed a null device).
    set(SOLOUD_BACKEND_ALSA ON CACHE BOOL "" FORCE)
elseif(ANDROID)
    # Track M: OpenSLES is the Android backend — NDK has no ALSA/WASAPI,
    # and AUTO picks nothing (same silent-init-failure pattern as Linux).
    set(SOLOUD_BACKEND_OPENSLES ON CACHE BOOL "" FORCE)
elseif(APPLE)
    # CoreAudio serves BOTH iOS and macOS desktop — AUTO picks nothing and
    # audio init fails with error 7 (same silent break pattern as Linux/ALSA).
    # macOS desktop was deliberately backend-less until Sprint6-L2 because
    # CoreAudio init on headless CI runners broke the http smoke; the engine
    # now degrades LOUDLY to NullAudio when the real backend fails to init
    # (Engine audio fallback), so real Macs get sound while headless CI still
    # boots. First evidence a backend-less mac package cannot boot at all:
    # CI run 33194926388, macOS · Package verify probe (SoLoud init failed: 7).
    set(SOLOUD_BACKEND_COREAUDIO ON CACHE BOOL "" FORCE)
endif()

add_subdirectory(${CMAKE_SOURCE_DIR}/external/bgfx/bx   ${CMAKE_BINARY_DIR}/bx)
add_subdirectory(${CMAKE_SOURCE_DIR}/external/bgfx/bimg ${CMAKE_BINARY_DIR}/bimg)
# bimg uses SSE4.1 intrinsics ( _mm_blendv_ps ) — enable on x86 Linux
if(UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
  target_compile_options(bimg PRIVATE -msse4.1)
endif()
add_subdirectory(${CMAKE_SOURCE_DIR}/external/bgfx/bgfx ${CMAKE_BINARY_DIR}/bgfx_lib)

add_subdirectory(${CMAKE_SOURCE_DIR}/external/soloud/contrib ${CMAKE_BINARY_DIR}/soloud)
add_subdirectory(${CMAKE_SOURCE_DIR}/external/lua ${CMAKE_BINARY_DIR}/lua)

# -- ed25519 (public-domain Ed25519 implementation) -------------------------
set(ED25519_SOURCES
    external/ed25519/fe.c
    external/ed25519/ge.c
    external/ed25519/keypair.c
    external/ed25519/sc.c
    external/ed25519/sha512.c
    external/ed25519/sign.c
    external/ed25519/verify.c
    external/ed25519/seed.c
    external/ed25519/add_scalar.c
)
add_library(ed25519 STATIC ${ED25519_SOURCES})
target_include_directories(ed25519 PUBLIC external/ed25519)
target_compile_definitions(ed25519 PRIVATE _CRT_SECURE_NO_WARNINGS)
# LPeg is pure-Lua (scripts/lpeg.lua) — no C build needed

# -- Engine modules ----------------------------------------------------------
# CAESURA_DEBUG follows the existing single-config default. For multi-config
# generators, Debug is enabled per configuration below; explicitly setting the
# option ON continues to force debug logging in every configuration.
if(CMAKE_BUILD_TYPE MATCHES "Debug")
    option(CAESURA_DEBUG "Enable debug logging and assertions" ON)
else()
    option(CAESURA_DEBUG "Enable debug logging and assertions" OFF)
endif()
option(CAESURA_HAS_STEAM "Enable Steamworks integration" OFF)

include(cmake/CaesuraModules.cmake)

# CMAKE_BUILD_TYPE is empty for Visual Studio and other multi-config
# generators. Add the default Debug definition at generation time without
# leaking it into Release-family configurations.
if(CMAKE_CONFIGURATION_TYPES AND NOT CAESURA_DEBUG)
    target_compile_definitions(CaesuraBuildOptions INTERFACE
        $<$<CONFIG:Debug>:CAESURA_DEBUG=1>
    )
endif()

# The distributable remains one executable; production sources live in the
# module libraries above and are compiled exactly once.
if(ANDROID)
    # Track M A2/R1: Android ships a JNI shared library that SDLActivity
    # loads, not a desktop executable. MODULE yields libCaesuraAmeKAG.so;
    # the final name the activity loads is set by gradle meta-data
    # (android.app.lib_name) when the A2 host module lands.
    add_library(${PROJECT_NAME} MODULE src/main.cpp)
else()
    add_executable(${PROJECT_NAME} src/main.cpp)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE Caesura::Engine Caesura::Rpc)
if(WIN32)
    # Preserve the host public-key path from the native wide command line.
    target_link_libraries(${PROJECT_NAME} PRIVATE shell32)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.caesura.amekag"
        MACOSX_BUNDLE_BUNDLE_NAME "CaesuraAmeKAG"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.caesura.amekag"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
    )
endif()

# -- FFmpeg (apply if found above) -----------------------------------------
if(CAESURA_FFMPEG_FOUND)
    # VideoPlayer.h changes layout under this flag, so consumers must inherit
    # the same definition as the render module.
    target_compile_definitions(CaesuraRender PUBLIC CAESURA_VIDEO_FFMPEG)
    target_include_directories(CaesuraRender PRIVATE ${CAESURA_FFMPEG_INCLUDE_DIRS})
    target_link_libraries(CaesuraRender PRIVATE ${CAESURA_FFMPEG_LIBRARIES})
    # Copy FFmpeg DLLs to output at configure time (avoids MSBuild Unicode path issues)
    if(WIN32)
        # Copy only the DLLs the engine links (avcodec/avformat/avutil/
        # swscale/swresample); avfilter/avdevice and the CLI tools are unused.
        set(CAESURA_FFMPEG_DLLS
            avcodec-62.dll avformat-62.dll avutil-60.dll
            swscale-9.dll swresample-6.dll)
        foreach(CFG Debug Release RelWithDebInfo MinSizeRel)
            foreach(DLL ${CAESURA_FFMPEG_DLLS})
                if(EXISTS "${FFMPEG_BIN_DIR}/${DLL}")
                    file(COPY "${FFMPEG_BIN_DIR}/${DLL}" DESTINATION ${CMAKE_BINARY_DIR}/${CFG})
                endif()
            endforeach()
        endforeach()
    endif()
endif()

# Copy SDL3 DLL to output directory for runtime. Only meaningful when the
# target resolves to a real library file (bundled SDL3); package-manager
# SDL3 (vcpkg) already installs the DLL alongside the build outputs.
if(TARGET SDL3::SDL3)
    get_target_property(_sdl3_type SDL3::SDL3 TYPE)
    if(_sdl3_type MATCHES "SHARED_LIBRARY|STATIC_LIBRARY|MODULE_LIBRARY")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SDL3::SDL3>"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/$<TARGET_FILE_NAME:SDL3::SDL3>"
            COMMENT "Copying SDL3 runtime to output"
            VERBATIM
        )
    endif()
endif()



if(MSVC)
    set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()


# Copy scripts to each config output directory
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/scripts"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/scripts"
    COMMENT "Copying scripts to output directory..."
)

# Copy demo assets to output directory (separated from engine source)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/demo"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/demo"
    COMMENT "Copying demo to output directory..."
)

# Copy engine assets (fonts, etc.) to output directory
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/assets"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
    COMMENT "Copying assets to output directory..."
)

# Copy the prebuilt web player so the editor package/web endpoint and the
# HTTP smoke can assemble a site from the output dir (same pattern as demo).
if(EXISTS "${CMAKE_SOURCE_DIR}/web/dist/index.html")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/web/dist"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/web/dist"
        COMMENT "Copying web player to output directory..."
    )
endif()

# Copy PM-created projects to output (may be empty at first)
file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/projects")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/projects"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/projects"
    COMMENT "Copying projects to output directory..."
)

# -- Steam (optional)
if(CAESURA_HAS_STEAM)
    # PUBLIC on purpose: SteamBackend.h changes SteamBackend's class layout
    # under this flag, so every consumer compiling that header (CaesuraEntry,
    # CaesuraTests via Caesura::Engine) must inherit the same definition --
    # same rationale as CAESURA_VIDEO_FFMPEG being PUBLIC on CaesuraRender.
    target_compile_definitions(CaesuraSteam PUBLIC CAESURA_HAS_STEAM)
    target_include_directories(CaesuraSteam PRIVATE ${STEAM_INCLUDE_DIR})
    target_link_libraries(CaesuraSteam PRIVATE ${STEAM_LIBRARY})
    # Copy the Steamworks runtime DLL next to the exe at configure time,
    # mirroring the FFmpeg DLL handling above; without it SteamAPI_Init fails
    # to load steam_api64.dll at runtime. The SDK ships it beside the import
    # library in redistributable_bin/<arch>/, so derive it from STEAM_LIBRARY.
    if(WIN32)
        get_filename_component(STEAM_SDK_BIN_DIR "${STEAM_LIBRARY}" DIRECTORY)
        foreach(CFG Debug Release RelWithDebInfo MinSizeRel)
            if(EXISTS "${STEAM_SDK_BIN_DIR}/steam_api64.dll")
                file(COPY "${STEAM_SDK_BIN_DIR}/steam_api64.dll" DESTINATION ${CMAKE_BINARY_DIR}/${CFG})
            endif()
        endforeach()
    endif()
endif()

# -- Live2D Cubism animation (optional, proprietary SDK) ---------------------
option(CAESURA_LIVE2D "Enable Live2D Cubism animation (requires manual SDK download)" OFF)

if(CAESURA_LIVE2D)
    # Prefer an explicit -DCUBISM_SDK_ROOT; otherwise probe the repo-local
    # thirdparty/ location (user-installed SDK, gitignored), then the root.
    if(NOT CUBISM_SDK_ROOT)
        if(EXISTS "${CMAKE_SOURCE_DIR}/thirdparty/CubismSdkForNative-5-r.5/Core/include/Live2DCubismCore.h")
            set(CUBISM_SDK_ROOT "${CMAKE_SOURCE_DIR}/thirdparty/CubismSdkForNative-5-r.5" CACHE PATH "Path to Live2D Cubism SDK root" FORCE)
        else()
            set(CUBISM_SDK_ROOT "${CMAKE_SOURCE_DIR}/CubismSdkForNative-5-r.5" CACHE PATH "Path to Live2D Cubism SDK root")
        endif()
    endif()

    if(NOT EXISTS "${CUBISM_SDK_ROOT}/Core/include/Live2DCubismCore.h")
        message(WARNING "Cubism SDK not found at ${CUBISM_SDK_ROOT}. Download from live2d.com")
    else()
        set(CUBISM_CORE_DIR "${CUBISM_SDK_ROOT}/Core")
        set(CUBISM_FRAMEWORK_DIR "${CUBISM_SDK_ROOT}/Framework")
        set(CAESURA_CUBISM_ARCH "${CMAKE_SYSTEM_PROCESSOR}" CACHE STRING
            "Cubism Core architecture directory (for example x86_64 or arm64)")
        set(CUBISM_ENGINE_RENDER_PATH_SRCS)
        set(CUBISM_OPENGL_RENDER_PATH_SRCS
            src/live2d/Live2D/OpenGLReadbackRenderPath.h
            src/live2d/Live2D/OpenGLReadbackRenderPath.cpp
            src/live2d/Live2D/OpenGLSharedRenderPath.h
            src/live2d/Live2D/OpenGLSharedRenderPath.cpp
        )
        set(CUBISM_PLATFORM_DEFINITION)
        set(CUBISM_PLATFORM_LIBRARIES)

        # --- Cubism Core (prebuilt) ---
        add_library(Live2DCubismCore STATIC IMPORTED)
        if(WIN32)
            if(NOT MSVC)
                message(FATAL_ERROR "Live2D Cubism Core on Windows requires MSVC")
            endif()
            set_target_properties(Live2DCubismCore PROPERTIES
                IMPORTED_LOCATION "${CUBISM_CORE_DIR}/lib/windows/x86_64/143/Live2DCubismCore_MT.lib"
                IMPORTED_LOCATION_DEBUG "${CUBISM_CORE_DIR}/lib/windows/x86_64/143/Live2DCubismCore_MTd.lib"
                INTERFACE_INCLUDE_DIRECTORIES "${CUBISM_CORE_DIR}/include"
            )
            set(CUBISM_FRAMEWORK_RENDERER D3D11)
            list(APPEND CUBISM_PLATFORM_LIBRARIES d3d11 d3dcompiler)
            list(APPEND CUBISM_ENGINE_RENDER_PATH_SRCS
                src/live2d/Live2D/D3D11NativeRenderPath.h
                src/live2d/Live2D/D3D11NativeRenderPath.cpp
            )
        elseif(APPLE)
            set_target_properties(Live2DCubismCore PROPERTIES
                IMPORTED_LOCATION "${CUBISM_CORE_DIR}/lib/macos/${CAESURA_CUBISM_ARCH}/libLive2DCubismCore.a"
                INTERFACE_INCLUDE_DIRECTORIES "${CUBISM_CORE_DIR}/include"
            )
            set(CUBISM_FRAMEWORK_RENDERER OpenGL)
            set(CUBISM_PLATFORM_DEFINITION CSM_TARGET_MAC_GL)
            list(APPEND CUBISM_ENGINE_RENDER_PATH_SRCS
                ${CUBISM_OPENGL_RENDER_PATH_SRCS}
                src/live2d/Live2D/MetalNativeRenderPath.h
                src/live2d/Live2D/MetalNativeRenderPath.cpp
            )
            # Metal render path uses Objective-C++ (id<MTLDevice> etc.).
            set_source_files_properties(
                ${CMAKE_SOURCE_DIR}/src/live2d/Live2D/MetalNativeRenderPath.cpp
                PROPERTIES LANGUAGE OBJCXX
            )
        elseif(UNIX)
            set_target_properties(Live2DCubismCore PROPERTIES
                IMPORTED_LOCATION "${CUBISM_CORE_DIR}/lib/linux/${CAESURA_CUBISM_ARCH}/libLive2DCubismCore.a"
                INTERFACE_INCLUDE_DIRECTORIES "${CUBISM_CORE_DIR}/include"
            )
            set(CUBISM_FRAMEWORK_RENDERER OpenGL)
            set(CUBISM_PLATFORM_DEFINITION CSM_TARGET_LINUX_GL)
            list(APPEND CUBISM_ENGINE_RENDER_PATH_SRCS
                ${CUBISM_OPENGL_RENDER_PATH_SRCS}
            )
        else()
            message(FATAL_ERROR "Live2D is not configured for ${CMAKE_SYSTEM_NAME}")
        endif()

        if(NOT WIN32)
            find_package(OpenGL REQUIRED)
            find_package(GLEW REQUIRED)
            list(APPEND CUBISM_PLATFORM_LIBRARIES OpenGL::GL GLEW::GLEW)
        endif()

        # --- Cubism Framework + one platform renderer ---
        file(GLOB_RECURSE CUBISM_FRAMEWORK_SRCS
            "${CUBISM_FRAMEWORK_DIR}/src/CubismCdiJson.cpp"
            "${CUBISM_FRAMEWORK_DIR}/src/CubismDefaultParameterId.cpp"
            "${CUBISM_FRAMEWORK_DIR}/src/CubismFramework.cpp"
            "${CUBISM_FRAMEWORK_DIR}/src/CubismModelSettingJson.cpp"
            "${CUBISM_FRAMEWORK_DIR}/src/Effect/*.cpp"
            "${CUBISM_FRAMEWORK_DIR}/src/Id/*.cpp"
            "${CUBISM_FRAMEWORK_DIR}/src/Math/*.cpp"
            "${CUBISM_FRAMEWORK_DIR}/src/Model/*.cpp"
            "${CUBISM_FRAMEWORK_DIR}/src/Motion/*.cpp"
            "${CUBISM_FRAMEWORK_DIR}/src/Physics/*.cpp"
            "${CUBISM_FRAMEWORK_DIR}/src/Rendering/csmBlendMode.cpp"
            "${CUBISM_FRAMEWORK_DIR}/src/Rendering/CubismRenderer.cpp"
            "${CUBISM_FRAMEWORK_DIR}/src/Rendering/${CUBISM_FRAMEWORK_RENDERER}/*.cpp"
            "${CUBISM_FRAMEWORK_DIR}/src/Type/*.cpp"
            "${CUBISM_FRAMEWORK_DIR}/src/Utils/*.cpp"
        )

        add_library(CubismFramework STATIC ${CUBISM_FRAMEWORK_SRCS})
        target_include_directories(CubismFramework PUBLIC
            "${CUBISM_FRAMEWORK_DIR}/src"
            "${CUBISM_CORE_DIR}/include"
        )
        target_include_directories(CubismFramework PRIVATE
            "${CUBISM_FRAMEWORK_DIR}/src/Rendering/${CUBISM_FRAMEWORK_RENDERER}"
        )
        target_link_libraries(CubismFramework PRIVATE
            Live2DCubismCore
            ${CUBISM_PLATFORM_LIBRARIES}
        )
        if(CUBISM_PLATFORM_DEFINITION)
            target_compile_definitions(CubismFramework PUBLIC
                ${CUBISM_PLATFORM_DEFINITION}
            )
        endif()
        if(MSVC)
            target_compile_definitions(CubismFramework PRIVATE
                _CRT_SECURE_NO_WARNINGS
            )
        endif()

        # --- Engine Live2D backend ---
        target_sources(CaesuraLive2D PRIVATE
            src/live2d/Live2D/Live2DBackend.h
            src/live2d/Live2D/Live2DBackend.cpp
            src/live2d/Live2D/ILive2DRenderPath.h
            ${CUBISM_ENGINE_RENDER_PATH_SRCS}
        )
        target_link_libraries(CaesuraLive2D PRIVATE
            CubismFramework
            Live2DCubismCore
            ${CUBISM_PLATFORM_LIBRARIES}
        )
        target_include_directories(CaesuraLive2D PRIVATE
            "${CUBISM_CORE_DIR}/include"
            "${CUBISM_FRAMEWORK_DIR}/src"
        )
        target_compile_definitions(CaesuraLive2D PRIVATE CAESURA_LIVE2D)
        target_compile_definitions(CaesuraEntry PRIVATE CAESURA_LIVE2D)
        message(STATUS "Live2D: ${CUBISM_FRAMEWORK_RENDERER} renderer for ${CMAKE_SYSTEM_NAME}/${CAESURA_CUBISM_ARCH}")

        # Cubism renderers load FrameworkShaders/* at runtime (relative to
        # the working directory); copy the shaders for the ACTIVE renderer:
        # D3D11 loads .fx, OpenGL loads .vert/.frag (C1: the GL copy was
        # missing -- models would render white/nothing on Linux/macOS).
        # The engine chdir()s to the source root at startup, so the shaders
        # must ALSO live there (not just next to the executable).
        set(CUBISM_SHADER_SRC_DIR "${CUBISM_FRAMEWORK_DIR}/src/Rendering/${CUBISM_FRAMEWORK_RENDERER}/Shaders")
        if(EXISTS "${CUBISM_SHADER_SRC_DIR}")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CUBISM_SHADER_SRC_DIR}"
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/FrameworkShaders"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CUBISM_SHADER_SRC_DIR}"
                    "${CMAKE_SOURCE_DIR}/FrameworkShaders"
                COMMENT "Copying Cubism FrameworkShaders (${CUBISM_FRAMEWORK_RENDERER}) to output + source root...")
        endif()
    endif()
endif()
message(STATUS "Caesura (AmeKAG) configured successfully.")



# -- CPack Release packaging ------------------------------------------------
# APPLE (macOS/iOS) build the executable as a MACOSX_BUNDLE, so CPack
# needs a BUNDLE destination (the iOS probe red point at the install step).
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
    BUNDLE DESTINATION .
)
install(DIRECTORY scripts/ DESTINATION scripts)
install(DIRECTORY demo/ DESTINATION demo)
install(DIRECTORY assets/ DESTINATION assets)
install(DIRECTORY projects/ DESTINATION projects)
install(DIRECTORY shaders/ DESTINATION shaders OPTIONAL)
# Project Manager templates are part of the release contract: the stranger
# path is "unzip -> caesura.py create" and the editor Project Manager, both of
# which resolve tools/project_templates/ next to the executable
# (docs/guides/release-process.md §5). Ship ONLY project_templates/ here --
# the tools/ directory also contains carc_pack sources/build junk.
install(DIRECTORY tools/project_templates/ DESTINATION tools/project_templates)
# "Unzip -> caesura.py build" must not require a system Lua: caesura_build.py's
# find_lua() probes <package>/external/lua/lua.exe then .../lua before falling
# back to PATH. Install the lua_cli TARGET (built at build/lua/<cfg>/lua[.exe])
# rather than external/lua/lua.exe -- that path is a gitignored, hand-built
# leftover which does NOT exist in a fresh clone, so a file-based install would
# break CI packaging. TARGETS also lands the right name per platform (lua.exe /
# lua), matching both find_lua() probe names.
# BUNDLE DESTINATION: iOS forces executable targets to MACOSX_BUNDLE, and an
# install(TARGETS <exe>) without it is a CONFIGURE error there (same red point
# the engine target hit above; the audit probe caught lua_cli on 33169661969).
install(TARGETS lua_cli RUNTIME DESTINATION external/lua BUNDLE DESTINATION external/lua)
# The editor frontend is part of the release contract: getting-started.md tells a
# stranger to unzip and run "CaesuraAmeKAG.exe --editor". Ship dist/ ONLY, never
# web-editor/ wholesale (node_modules/ and release/ are build junk). Not OPTIONAL
# on purpose: dist/index.html is a TRACKED file, so a fresh clone always has it,
# and if it ever disappears the package must fail loudly rather than quietly ship
# an engine whose editor answers 404. The trailing slash matters -- without it
# CMake produces web-editor/dist/dist/.
install(DIRECTORY web-editor/dist/ DESTINATION web-editor/dist)
# editor/ (React/Monaco IDE, Studio Phase 1) is the opposite case: editor/dist
# is gitignored, so a fresh clone never has it and install() must not fail the
# package build on machines that only build the engine. OPTIONAL skips it
# silently -- the loud "it should have been there" sentinel lives in
# scripts/verify_release_package.sh (editor/dist/index.html + assets/*.js), and
# the release/ci packaging jobs run `npm run build` in editor/ before cpack.
install(DIRECTORY editor/dist/ DESTINATION editor/dist OPTIONAL)
install(FILES README.md LICENSE DESTINATION . OPTIONAL)

set(CPACK_PACKAGE_NAME "CaesuraAmeKAG")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR "AiliasDesu")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Cross-Platform Visual Novel Engine — SDL3 + bgfx + SoLoud + Lua 5.4")
# Sprint6-L1: every non-Windows platform ships a TGZ -- tar preserves the
# executable bit, which a ZIP extracted with unzip loses (t29: this also
# covers the future macOS lane with zero further CMake churn).
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)

# Deterministic, human-readable artifact name (version + platform): e.g.
# CaesuraAmeKAG-1.0.0-win64.zip. Keeps the CI artifact copy-paste stable.
set(CPACK_PACKAGE_FILE_NAME
    "CaesuraAmeKAG-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

# Emit a SHA256 sidecar next to the ZIP (e.g. ..-win64.zip.sha256).
set(CPACK_PACKAGE_CHECKSUM "SHA256")

# Resolve-and-embed the license file for package metadata/diagnostics.
if(EXISTS "${CMAKE_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
endif()

# Bundle the SDL3 runtime DLL on Windows only (the release artifact and CI
# release job are Windows-targeted). On Linux/macOS TARGET_FILE resolves to a
# platform native shared lib (libSDL3.so / .dylib); gate this to WIN32 to keep
# the ZIP content uniform, mirroring the FFmpeg DLL gate below.
if(WIN32 AND TARGET SDL3::SDL3)
    install(FILES "$<TARGET_FILE:SDL3::SDL3>" DESTINATION . OPTIONAL)
endif()
# Sprint6-L1: the Linux artifact must carry its own SDL3 -- CI builds SDL3
# from source into /usr/local, and a stranger's distro has no libSDL3
# package to fall back on (Ubuntu 24.04 ships none). Install the SONAME
# file: that is the exact name the loader requests (install(FILES)
# dereferences the symlink, so the package carries a real libSDL3.so.0),
# and the engine finds it beside the executable through $ORIGIN below.
if(UNIX AND NOT APPLE AND TARGET SDL3::SDL3)
    get_target_property(_caesura_sdl3_type SDL3::SDL3 TYPE)
    if(_caesura_sdl3_type STREQUAL "SHARED_LIBRARY")
        install(FILES "$<TARGET_SONAME_FILE:SDL3::SDL3>" DESTINATION . OPTIONAL)
    endif()
    set_target_properties(${PROJECT_NAME} PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
# Sprint6-L2: macOS mirrors the Linux self-carry rule. CI builds SDL3 from
# source (its dylib id is @rpath/libSDL3.<v>.dylib), the package ships the
# SONAME dylib beside the executable, and @loader_path plays the role of
# $ORIGIN. brew's SDL3 is NOT a supported package source: its absolute
# install_name (/opt/homebrew/...) would pin the binary to the build machine.
# iOS packaging is a separate lane (bundle layout), hence the explicit
# CMAKE_SYSTEM_NAME guard (repo convention; version-independent, unlike the
# CMake>=3.14 builtin IOS variable).
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND TARGET SDL3::SDL3)
    get_target_property(_caesura_sdl3_type_mac SDL3::SDL3 TYPE)
    if(_caesura_sdl3_type_mac STREQUAL "SHARED_LIBRARY")
        install(FILES "$<TARGET_SONAME_FILE:SDL3::SDL3>" DESTINATION . OPTIONAL)
    endif()
    set_target_properties(${PROJECT_NAME} PROPERTIES INSTALL_RPATH "@loader_path")
endif()
# Without the Steam runtime DLL beside the executable the process cannot even
# START ("error while loading shared libraries: steam_api64.dll") -- exactly what
# a stranger hits after unzipping a Steam-enabled build. The configure-time copy
# above only populates build/<cfg>/, not the package. OPTIONAL because the SDK
# path is user-supplied.
if(CAESURA_HAS_STEAM AND WIN32 AND STEAM_LIBRARY)
    get_filename_component(CAESURA_STEAM_BIN_DIR "${STEAM_LIBRARY}" DIRECTORY)
    install(FILES "${CAESURA_STEAM_BIN_DIR}/steam_api64.dll" DESTINATION . OPTIONAL)
endif()
if(CAESURA_FFMPEG_FOUND AND WIN32)
    install(FILES
        ${FFMPEG_BIN_DIR}/avcodec-62.dll
        ${FFMPEG_BIN_DIR}/avformat-62.dll
        ${FFMPEG_BIN_DIR}/avutil-60.dll
        ${FFMPEG_BIN_DIR}/swscale-9.dll
        ${FFMPEG_BIN_DIR}/swresample-6.dll
        DESTINATION . OPTIONAL)
endif()

include(CPack)

# -- Tools ----------------------------------------------------------------
add_subdirectory(tools/carc_pack)

# -- Unit Tests
enable_testing()
add_subdirectory(tests)
