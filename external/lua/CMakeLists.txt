# Lua 5.4.7 — static library
cmake_minimum_required(VERSION 3.25)
project(lua_lib C CXX)

set(LUA_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(LUA_SOURCES
    ${LUA_DIR}/lapi.c ${LUA_DIR}/lauxlib.c ${LUA_DIR}/lbaselib.c
    ${LUA_DIR}/lcode.c ${LUA_DIR}/lcorolib.c ${LUA_DIR}/lctype.c
    ${LUA_DIR}/ldblib.c ${LUA_DIR}/ldebug.c ${LUA_DIR}/ldo.c
    ${LUA_DIR}/ldump.c ${LUA_DIR}/lfunc.c ${LUA_DIR}/lgc.c
    ${LUA_DIR}/linit.c ${LUA_DIR}/liolib.c ${LUA_DIR}/llex.c
    ${LUA_DIR}/lmathlib.c ${LUA_DIR}/lmem.c ${LUA_DIR}/loadlib.c
    ${LUA_DIR}/lobject.c ${LUA_DIR}/lopcodes.c ${LUA_DIR}/loslib.c
    ${LUA_DIR}/lparser.c ${LUA_DIR}/lstate.c ${LUA_DIR}/lstring.c
    ${LUA_DIR}/lstrlib.c ${LUA_DIR}/ltable.c ${LUA_DIR}/ltablib.c
    ${LUA_DIR}/ltm.c ${LUA_DIR}/lundump.c ${LUA_DIR}/lutf8lib.c
    ${LUA_DIR}/lvm.c ${LUA_DIR}/lzio.c
)

add_library(lua STATIC ${LUA_SOURCES})
# LNK1149 guard (round 70): MSVC compares link inputs/outputs by BASE
# name, so an interpreter named lua.exe cannot sit next to the static
# lib lua.lib — rename the archive output to lua_lib.lib (consumers
# link the TARGET name, which is unchanged).
set_target_properties(lua PROPERTIES OUTPUT_NAME lua_lib)
target_include_directories(lua PUBLIC ${LUA_DIR})

if(MSVC)
    target_compile_options(lua PRIVATE /W0)
    target_compile_definitions(lua PRIVATE _CRT_SECURE_NO_WARNINGS LUA_BUILD_AS_DLL=0)
else()
    target_compile_options(lua PRIVATE -w)
endif()

# Desktop flags enable POSIX process/status handling and the native loader in
# both the library and its interpreter. Android keeps its existing setup.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    # The luaconf LUA_USE_IOS guard stubs l_system(): iOS has no system().
    target_compile_definitions(lua PRIVATE LUA_USE_IOS)
    if(TARGET lua_cli)
        target_compile_definitions(lua_cli PRIVATE LUA_USE_IOS)
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(lua PUBLIC LUA_USE_LINUX)
    # Static-library consumers need these too, not just lua_cli. CMake picks
    # the platform's dl library; libm supplies Lua's pow/floor implementations.
    target_link_libraries(lua PUBLIC ${CMAKE_DL_LIBS} m)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # macOS libSystem already provides the loader and math functions.
    target_compile_definitions(lua PUBLIC LUA_USE_MACOSX)
endif()

# Round 70: standalone interpreter for the script test suites. The engine
# links the static lib; the test tooling (run_lua_tests.lua, ks_check.lua,
# api_stats census) needs a runnable lua — previously only a gitignored
# prebuilt binary existed, so Windows CI SILENTLY SKIPPED the Lua suites
# and the docs-freshness guard failed there. Building lua.c gives every
# platform a real interpreter (build/lua/<config>/lua[.exe]).
# CLI-only preload: engine lua_lib/linit.c are unchanged. Reuse the bounded
# directory implementation directly, without linking the Engine/Resource graph.
add_executable(lua_cli
    ${LUA_DIR}/../../tools/lua_cli_main.c
    ${LUA_DIR}/../../tools/lua_cli_fs.cpp
    ${LUA_DIR}/../../src/resource/DirAssetProvider.cpp)
target_include_directories(lua_cli PRIVATE ${LUA_DIR}/../../src)
target_compile_features(lua_cli PRIVATE cxx_std_20)
target_link_libraries(lua_cli PRIVATE lua)
if(MSVC)
    target_compile_options(lua_cli PRIVATE /W0)
    target_compile_definitions(lua_cli PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(lua_cli PRIVATE -w)
    if(NOT APPLE)
        # lvm.c uses pow()/floor(); macOS libSystem provides them, Linux
        # needs an explicit libm link or the linker reports undefined refs.
        target_link_libraries(lua_cli PRIVATE m)
    endif()
endif()
set_target_properties(lua_cli PROPERTIES OUTPUT_NAME lua)
