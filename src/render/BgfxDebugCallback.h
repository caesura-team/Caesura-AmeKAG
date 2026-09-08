#pragma once

#include <bgfx/bgfx.h>
#include <cstdint>  // fixed-width types (GCC strict)
#include <bx/bx.h>
#include "../debug/api/DebugLog.h"  // P1-6: api header; thread-safe sink (mutex-guarded)
#include <cstdio>
#include <cstdarg>
#include <atomic>
#include "ScreenshotQueue.h"

// BgfxDebugCallback -- captures bgfx internal error/warning messages.
// Extracted from BgfxRenderDevice (U1).

class BgfxDebugCallback : public bgfx::CallbackI {
public:
    explicit BgfxDebugCallback(std::shared_ptr<Caesura::ScreenshotQueue> queue = std::make_shared<Caesura::ScreenshotQueue>())
        : m_screenshots(std::move(queue)) {}
    void reset() { m_shuttingDown.store(false); m_deviceLost.store(false); m_lossPending.store(false); }
    void beginShutdown() { m_shuttingDown.store(true); m_screenshots->close("renderer shutting down"); }

    // Device loss detection (set from bgfx fatal callback on any thread)
    void flagDeviceLost() {
        m_deviceLost.store(true);
        m_lossPending.store(true);
        m_screenshots->close("device lost");
    }
    bool deviceLost() const { return m_deviceLost.load(); }
    bool consumeDeviceLost() { return m_lossPending.exchange(false); }
    void fatal(const char* _filePath, uint16_t _line, bgfx::Fatal::Enum _code, const char* _str) override {
        // Device lost is non-fatal — flag it and let the main loop recover
        if (_code == bgfx::Fatal::DeviceLost) {
            DEBUG_ERR(Caesura::SubSys::Render, Caesura::ErrCode::Ok,
                      "[BgfxDebugCallback] Device lost detected (code=%d: %s)",
                      (int)_code, _str);
            flagDeviceLost();
            return;
        }
        BX_UNUSED(_code);
        if (!m_shuttingDown.load()) {
            DEBUG_WARN(Caesura::SubSys::Render, Caesura::ErrCode::Ok,
                       "[bgfx WARN] %s(%d): %s",
                       _filePath, (int)_line, _str);
        }
    }
    void traceVargs(const char* _filePath, uint16_t _line, const char* _format, va_list _argList) override {
        char buf[2048];
        vsnprintf(buf, sizeof(buf), _format, _argList);
        DEBUG_DBG(Caesura::SubSys::Render, Caesura::ErrCode::Ok, "[bgfx] %s(%d): %s",
                  _filePath, (int)_line, buf);
    }
    void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerEnd() override {}
    uint32_t cacheReadSize(uint64_t) override { return 0; }
    bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
    void cacheWrite(uint64_t, const void*, uint32_t) override {}
    void screenShot(const char* _name, uint32_t _width, uint32_t _height,
                    uint32_t _pitch, bgfx::TextureFormat::Enum _format,
                    const void* _data, uint32_t _size, bool _flipY) override;
    void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
    void captureEnd() override {}
    void captureFrame(const void*, uint32_t) override {}
private:
    std::shared_ptr<Caesura::ScreenshotQueue> m_screenshots;
    std::atomic<bool> m_shuttingDown{false};
    std::atomic<bool> m_deviceLost{false};
    std::atomic<bool> m_lossPending{false};
};
