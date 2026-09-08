#define DOCTEST_CONFIG_IMPLEMENT
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#define WIN32_LEAN_AND_MEAN
#include "doctest.h"
#include "entry/Engine.h"
#include <SDL3/SDL_main.h>
#include <thread>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#ifdef _WIN32
static void printStackTrace() {
    void* stack[64];
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, NULL, TRUE);
    WORD frames = CaptureStackBackTrace(0, 64, stack, NULL);
    for (WORD i = 0; i < frames; i++) {
        DWORD64 addr = (DWORD64)stack[i];
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp;
        if (SymFromAddr(process, addr, &disp, sym)) {
            fprintf(stderr, "  #%d: %s+0x%llx\n", i, sym->Name, disp);
        } else {
            fprintf(stderr, "  #%d: 0x%llx\n", i, addr);
        }
    }
    SymCleanup(process);
}
#endif

#ifdef _MSC_VER
static int __cdecl reportHook(int reportType, char* message, int* returnValue) {
    if (reportType == _CRT_ASSERT) {
        fprintf(stderr, "ASSERT: %s\n", message);
        printStackTrace();
    }
    return 0;
}

static int s_setupCRT = []() -> int {
    _CrtSetReportHook(reportHook);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    return 0;
}();
#endif

static int s_testSetup = []() -> int {
    Caesura::detail::g_mainThreadId = std::this_thread::get_id();
    return 0;
}();

int main(int argc, char** argv) {
    // This console runner owns main; it does not enter SDL's UIKit app wrapper.
    SDL_SetMainReady();
    return doctest::Context(argc, argv).run();
}
