#include "doctest.h"

#if defined(CAESURA_LIVE2D) && defined(_WIN32)
#include "HiddenGpuContext.h"
#include "live2d/Live2D/D3D11NativeRenderPath.h"

namespace Caesura {
struct D3D11LibraryLifetimeProbeAccess {
    static bool retain(D3D11NativeRenderPath& path) {
        return path.retainLoadedRuntime();
    }
};
}

TEST_CASE("Live2D D3D11 runtime reference survives external unload and retires once") {
    constexpr auto child = L"CAESURA_D3D11_LIBRARY_LIFETIME_CHILD";
    if (!CaesuraTest::isGpuChildProcess(child)) {
        // This existing helper owns a fresh test process; no window or GPU API
        // is used by this test. Isolation makes the loader refcount observable.
        CHECK(CaesuraTest::runGpuChildProcess(child,
            L"Live2D D3D11 runtime reference survives external unload and retires once") == 0);
        return;
    }
    REQUIRE(GetModuleHandleW(L"d3d11.dll") == nullptr);
    Caesura::D3D11NativeRenderPath path;
    CHECK_FALSE(Caesura::D3D11LibraryLifetimeProbeAccess::retain(path));
    path.shutdown();
    CHECK(GetModuleHandleW(L"d3d11.dll") == nullptr);

    const auto loaderReference = LoadLibraryExW(L"d3d11.dll", nullptr,
                                               LOAD_LIBRARY_SEARCH_SYSTEM32);
    REQUIRE(loaderReference != nullptr);
    REQUIRE(Caesura::D3D11LibraryLifetimeProbeAccess::retain(path));
    REQUIRE(Caesura::D3D11LibraryLifetimeProbeAccess::retain(path));
    REQUIRE(FreeLibrary(loaderReference) != 0);
    CHECK(GetModuleHandleW(L"d3d11.dll") == loaderReference);
    path.shutdown();
    CHECK(GetModuleHandleW(L"d3d11.dll") == nullptr);
    path.shutdown();
    CHECK(GetModuleHandleW(L"d3d11.dll") == nullptr);

    // Loaded runtime without a bgfx device: init must roll back its acquired
    // loader reference rather than retaining it after reporting failure.
    const auto unavailableDeviceLoader = LoadLibraryExW(L"d3d11.dll", nullptr,
                                                       LOAD_LIBRARY_SEARCH_SYSTEM32);
    REQUIRE(unavailableDeviceLoader != nullptr);
    CHECK_FALSE(path.init(1280, 720));
    REQUIRE(FreeLibrary(unavailableDeviceLoader) != 0);
    CHECK(GetModuleHandleW(L"d3d11.dll") == nullptr);

    // Partial initialization: library acquired but no context/resources yet.
    // Destruction must return the one retained loader reference automatically.
    const auto partialLoader = LoadLibraryExW(L"d3d11.dll", nullptr,
                                             LOAD_LIBRARY_SEARCH_SYSTEM32);
    REQUIRE(partialLoader != nullptr);
    {
        Caesura::D3D11NativeRenderPath partial;
        REQUIRE(Caesura::D3D11LibraryLifetimeProbeAccess::retain(partial));
        REQUIRE(FreeLibrary(partialLoader) != 0);
        CHECK(GetModuleHandleW(L"d3d11.dll") == partialLoader);
    }
    CHECK(GetModuleHandleW(L"d3d11.dll") == nullptr);
}
#endif
