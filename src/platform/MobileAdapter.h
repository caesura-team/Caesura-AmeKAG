// MobileAdapter -- Mobile platform adapter.
// Spec [10.2.64]: Touch input mapping, lifecycle events, DPI scaling.
// Engine maps native SDL contact identities and lifecycle events into this adapter.
// Namespace: Caesura (consistent with engine layering).
#pragma once
#include "api/IMobileAdapter.h"
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>

// Forward declaration for Lua state (avoid full include in header)
struct lua_State;
union SDL_Event;

namespace Caesura {

/// Touch point data for finger events
struct TouchPoint {
    float x = 0.0f;
    float y = 0.0f;
    int   fingerId = 0;
    bool  active = false;
};

/// Mobile platform adapter -- lifecycle + touch → input mapping.
/// Engine feeds SDL contacts through stable integer slots, classifies gestures,
/// and receives synthetic input synchronously. Standalone users retain the
/// default SDL queue route. Lifecycle callbacks live here; the composition root
/// owns audio suspension and independently paired focus/background reasons.
class MobileAdapter : public IMobileAdapter {
public:
    MobileAdapter() = default;
    ~MobileAdapter() override = default;

    // ── Lifecycle ──────────────────────────────────────────────────────

    /// Called when app goes to background.
    /// Publishes paused state and invokes Lua _G.onPause().
    void onPause(lua_State* L) override;

    /// Called when app returns to foreground.
    /// Publishes resumed state and invokes Lua _G.onResume(savedData).
    void onResume(lua_State* L, const std::string& savedData = "") override;

    /// Display orientation change -> Lua _G.onOrientationChanged(name).
    void onOrientationChanged(lua_State* L, const char* orientation) override;

    /// Track P2: OS memory pressure -> _G.onLowMemory() (safe no-op without L).
    void onLowMemory(lua_State* L) override;

    /// Track P2: termination notice -> _G.onTerminate() (safe no-op without L).
    void onTerminate(lua_State* L) override;

    // ── Touch → Mouse Mapping ──────────────────────────────────────────

    // Concrete host configuration; the public IMobileAdapter stays unchanged.
    // An empty sink preserves immediate SDL_PushEvent delivery. A configured
    // sink receives the same event synchronously, with no duplicate queueing.
    void setEventSink(std::function<void(const SDL_Event&)> sink);
    // Deferred mode classifies a complete contact sequence before left-click.
    // Changing mode cancels old contacts and releases any emitted left button.
    void setDeferredTouchClicks(bool enabled);
    void cancelTouches();

    /// Single finger down -- immediate left press unless deferred mode is set.
    void onFingerDown(float x, float y, int fingerId = 0) override;

    /// Finger moved -- maps to mouse motion at (x, y).
    void onFingerMotion(float x, float y, int fingerId = 0) override;

    /// Finger lifted -- immediate release, or a classified deferred tap pair.
    void onFingerUp(float x, float y, int fingerId = 0) override;

    // ── Gesture Input ──────────────────────────────────────────────────

    /// Pinch gesture -- zoom in/out.
    /// `scale` is the cumulative gesture scale (starts at 1.0f). Each call
    /// maps the scale delta to a vertical mouse-wheel event (zoom). Call
    /// `resetPinch()` when the gesture ends (all fingers lifted).
    void onPinch(float centerX, float centerY, float scale) override;

    /// End the active pinch gesture (resets the scale baseline).
    void resetPinch() override { m_lastPinchScale = 0.0f; }

    /// Current pinch scale baseline (0 = no active pinch gesture).
    float getLastPinchScale() const override { return m_lastPinchScale; }

    /// Long press -- maps to right mouse button click.
    /// Press-duration tracking (>500ms) is the platform layer's
    /// responsibility; this callback fires once when the press is detected.
    void onLongPress(float x, float y) override;

    // ── Multi-finger gestures (C6) ─────────────────────────────────────
    // Each of the four maps a gesture onto an EXISTING desktop input so the
    // touch path shares the keyboard/mouse handlers instead of growing a
    // parallel one. Consumer status verified by grepping the engine and the
    // Lua tree — two are live end to end, two are NOT WIRED yet and say so
    // rather than pretending to work:

    /// Two-finger tap -> right-click pair.
    /// WIRED: Engine.cpp dispatches SDL_BUTTON_RIGHT to _KAG_onRightClick.
    void onTwoFingerTap(float centerX, float centerY) override;

    /// Three-finger hold -> LCTRL keydown (skip mode).
    /// WIRED: Engine.cpp forwards LCTRL/RCTRL to _KAG_onCtrlDown, which is the
    /// same toggle a desktop Ctrl press performs (scripts/kag/quickmenu.lua
    /// owns skip_mode).
    void onThreeFingerHold(float centerX, float centerY) override;

    /// Swipe down -> SDLK_SPACE keydown.
    /// WIRED (t109): Engine.cpp's key-down handler routes SDLK_SPACE to the
    /// Lua hook _KAG_onKeySpace (guard-pattern same as _KAG_onCtrlDown), which
    /// toggles the message layer visibility -- mirroring the web gesture
    /// (web/main.mjs onSwipeDown hides/toggles the dialogue box).
    void onSwipeDown(float startX, float startY, float endX, float endY) override;

    /// Swipe up -> SDLK_PAGEUP keydown.
    /// WIRED (t109): Engine.cpp routes SDLK_PAGEUP to the Lua hook
    /// _KAG_onKeyPageUp (guard-pattern same as _KAG_onCtrlDown), which opens
    /// the backlog/history overlay -- mirroring the web gesture
    /// (web/main.mjs onSwipeUp shows + bottom-scrolls the backlog view).
    void onSwipeUp(float startX, float startY, float endX, float endY) override;

    // ── Display ────────────────────────────────────────────────────────

    /// Get display scale factor (DPI-based).
    /// Desktop returns 1.0; mobile returns actual DPI scale.
    float getDisplayScale() const override;

    /// Set display scale for testing. Non-finite values are rejected.
    void setDisplayScale(float scale) override {
        m_displayScale = std::isfinite(scale) ? scale : 1.0f;
    }

    // ── State ──────────────────────────────────────────────────────────

    /// Check if currently in background / paused.
    bool isPaused() const override { return m_paused; }

    /// Get the active touch point count.
    int activeTouchCount() const override { return m_activeTouches; }

    /// Check if a specific finger is currently down.
    bool isFingerDown(int fingerId) const override;

private:
    struct TouchOrigin {
        float x = 0.0f, y = 0.0f, maxTravelSq = 0.0f;
    };
    struct EmittedButton {
        bool down = false;
        uint64_t owner = 0;
        float x = 0.0f, y = 0.0f;
    };
    void emitEvent(const SDL_Event& event);
    EmittedButton& emittedButton(uint8_t button);
    uint64_t emitButtonDown(uint8_t button, float x, float y);
    void emitButtonUp(uint8_t button, float x, float y);
    void emitButtonPair(uint8_t button, float x, float y);
    void rememberTouchPosition(int fingerId, float x, float y);
    void consumeTouchTap();

    bool  m_paused = false;
    float m_displayScale = 1.0f;
    float m_lastPinchScale = 0.0f;
    int   m_activeTouches = 0;
    std::function<void(const SDL_Event&)> m_eventSink;
    bool m_deferredTouchClicks = false;
    bool m_sequenceTapSuppressed = false;
    // Contact state is retired BEFORE calling the sink. Button ownership is
    // independent: a new deferred contact still permits the old balancing up,
    // whereas a new emitted down supersedes the old pair's release authority.
    uint64_t m_buttonSerial = 0;
    EmittedButton m_leftButton, m_rightButton;

    /// Last known positions per finger (up to 10 simultaneous touches)
    static constexpr int MAX_TOUCH_POINTS = 10;
    TouchPoint m_touchPoints[MAX_TOUCH_POINTS];
    TouchOrigin m_touchOrigins[MAX_TOUCH_POINTS];
};

} // namespace Caesura
