#include "doctest.h"
#include "platform/MobileAdapter.h"
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <SDL3/SDL.h>
#include <cmath>
#include <algorithm>
#include <array>
#include <functional>
#include <utility>
#include <vector>

using namespace Caesura;

namespace {

// Synchronous event probe: SDL_PushEvent() invokes watchers registered with
// SDL_AddEventWatch() *before* queueing, so injection can be verified in
// unit tests without SDL_Init / a window.
struct EventProbe {
    SDL_Event last{};
    int count = 0;
    static bool SDLCALL watch(void* userdata, SDL_Event* ev) {
        auto* probe = static_cast<EventProbe*>(userdata);
        probe->last = *ev;
        probe->count++;
        return true;
    }
};

// RAII guard: the watch must be removed even when a REQUIRE fails and the
// test unwinds, otherwise SDL's global watcher list keeps a dangling pointer.
struct EventProbeGuard {
    EventProbe probe;
    EventProbeGuard() { SDL_AddEventWatch(EventProbe::watch, &probe); }
    ~EventProbeGuard() { SDL_RemoveEventWatch(EventProbe::watch, &probe); }
};

} // namespace

TEST_CASE("MobileAdapter::stub returns safe defaults") {
    MobileAdapter ma;
    CHECK_FALSE(ma.isPaused());
    CHECK(ma.activeTouchCount() == 0);
}

TEST_CASE("MobileAdapter::display scale") {
    MobileAdapter ma;
    CHECK(ma.getDisplayScale() == doctest::Approx(1.0f));
    ma.setDisplayScale(2.0f);
    CHECK(ma.getDisplayScale() == doctest::Approx(2.0f));
}

TEST_CASE("MobileAdapter::lifecycle stubs do not crash") {
    MobileAdapter ma;
    ma.onPause(nullptr);
    CHECK(ma.isPaused());
    ma.onResume(nullptr);
    CHECK_FALSE(ma.isPaused());
}

TEST_CASE("MobileAdapter::lifecycle resume with null state and saved data") {
    MobileAdapter ma;
    // No Lua state available in unit tests: nullptr must be safe and
    // the paused flag must still toggle.
    ma.onPause(nullptr);
    CHECK(ma.isPaused());
    ma.onResume(nullptr, "slot_3");
    CHECK_FALSE(ma.isPaused());
}

// =============================================================================
// Expanded: remaining stub methods
// =============================================================================

TEST_CASE("MobileAdapter::onFingerMotion does not crash") {
    MobileAdapter ma;
    CHECK_NOTHROW(ma.onFingerMotion(150.0f, 250.0f, 0));
}

TEST_CASE("MobileAdapter::onPinch does not crash") {
    MobileAdapter ma;
    CHECK_NOTHROW(ma.onPinch(100.0f, 100.0f, 1.5f));
}

TEST_CASE("MobileAdapter::onLongPress does not crash") {
    MobileAdapter ma;
    CHECK_NOTHROW(ma.onLongPress(200.0f, 300.0f));
}

TEST_CASE("MobileAdapter::isFingerDown tracks touch state") {
    MobileAdapter ma;
    CHECK_FALSE(ma.isFingerDown(0));
    ma.onFingerDown(50.0f, 50.0f, 0);
    CHECK(ma.isFingerDown(0));
    CHECK_FALSE(ma.isFingerDown(1));  // different finger
    ma.onFingerUp(50.0f, 50.0f, 0);
    CHECK_FALSE(ma.isFingerDown(0));
}

// =============================================================================
// Touch state machine (fixed in P4-3): counting must not drift
// =============================================================================

TEST_CASE("MobileAdapter::multi-touch counting") {
    MobileAdapter ma;
    ma.onFingerDown(10.0f, 10.0f, 0);
    ma.onFingerDown(20.0f, 20.0f, 1);
    ma.onFingerDown(30.0f, 30.0f, 2);
    CHECK(ma.activeTouchCount() == 3);
    CHECK(ma.isFingerDown(0));
    CHECK(ma.isFingerDown(1));
    CHECK(ma.isFingerDown(2));

    ma.onFingerUp(20.0f, 20.0f, 1);
    CHECK(ma.activeTouchCount() == 2);
    CHECK_FALSE(ma.isFingerDown(1));
    CHECK(ma.isFingerDown(0));
    CHECK(ma.isFingerDown(2));

    ma.onFingerUp(10.0f, 10.0f, 0);
    ma.onFingerUp(30.0f, 30.0f, 2);
    CHECK(ma.activeTouchCount() == 0);
}

TEST_CASE("MobileAdapter::duplicate finger down does not double-count") {
    MobileAdapter ma;
    ma.onFingerDown(10.0f, 10.0f, 0);
    ma.onFingerDown(15.0f, 15.0f, 0);  // duplicate down, same finger
    CHECK(ma.activeTouchCount() == 1);
    CHECK(ma.isFingerDown(0));
    ma.onFingerUp(15.0f, 15.0f, 0);
    CHECK(ma.activeTouchCount() == 0);
}

TEST_CASE("MobileAdapter::out-of-range fingers are ignored") {
    MobileAdapter ma;
    ma.onFingerDown(10.0f, 10.0f, 99);   // out of range
    ma.onFingerDown(10.0f, 10.0f, -1);   // negative id
    CHECK(ma.activeTouchCount() == 0);
    CHECK_FALSE(ma.isFingerDown(99));

    ma.onFingerUp(10.0f, 10.0f, 99);     // up without down, out of range
    CHECK(ma.activeTouchCount() == 0);   // must not go negative

    ma.onFingerDown(10.0f, 10.0f, 0);
    CHECK(ma.activeTouchCount() == 1);
    ma.onFingerUp(10.0f, 10.0f, 99);     // up of unknown finger
    CHECK(ma.activeTouchCount() == 1);   // real finger untouched
}

TEST_CASE("MobileAdapter::finger up without down does not underflow") {
    MobileAdapter ma;
    ma.onFingerUp(10.0f, 10.0f, 0);
    ma.onFingerUp(10.0f, 10.0f, 0);
    CHECK(ma.activeTouchCount() == 0);
}

// =============================================================================
// Event injection (verified synchronously via SDL_AddEventWatch)
// =============================================================================

TEST_CASE("MobileAdapter::touch injects scaled mouse events") {
    EventProbeGuard g;

    MobileAdapter ma;
    ma.setDisplayScale(2.0f);

    ma.onFingerDown(10.0f, 20.0f, 0);
    REQUIRE(g.probe.count == 1);
    CHECK(g.probe.last.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
    CHECK(g.probe.last.button.x == doctest::Approx(20.0f));
    CHECK(g.probe.last.button.y == doctest::Approx(40.0f));
    CHECK(g.probe.last.button.button == SDL_BUTTON_LEFT);

    ma.onFingerMotion(15.0f, 25.0f, 0);
    REQUIRE(g.probe.count == 2);
    CHECK(g.probe.last.type == SDL_EVENT_MOUSE_MOTION);
    CHECK(g.probe.last.motion.x == doctest::Approx(30.0f));
    CHECK(g.probe.last.motion.y == doctest::Approx(50.0f));

    ma.onFingerUp(15.0f, 25.0f, 0);
    REQUIRE(g.probe.count == 3);
    CHECK(g.probe.last.type == SDL_EVENT_MOUSE_BUTTON_UP);
    CHECK(g.probe.last.button.button == SDL_BUTTON_LEFT);
}

TEST_CASE("MobileAdapter::duplicate down and untracked motion inject nothing") {
    EventProbeGuard g;

    MobileAdapter ma;
    ma.onFingerDown(10.0f, 10.0f, 0);
    CHECK(g.probe.count == 1);

    ma.onFingerDown(11.0f, 11.0f, 0);   // duplicate down: no new event
    CHECK(g.probe.count == 1);

    ma.onFingerMotion(12.0f, 12.0f, 1); // motion of untracked finger: none
    CHECK(g.probe.count == 1);

    ma.onFingerMotion(12.0f, 12.0f, 0); // motion of tracked finger
    CHECK(g.probe.count == 2);
}

TEST_CASE("MobileAdapter::long press injects right-click pair") {
    EventProbeGuard g;

    MobileAdapter ma;
    ma.onLongPress(100.0f, 200.0f);
    REQUIRE(g.probe.count == 2);
    CHECK(g.probe.last.type == SDL_EVENT_MOUSE_BUTTON_UP);
    CHECK(g.probe.last.button.button == SDL_BUTTON_RIGHT);
}

// =============================================================================
// Pinch → wheel mapping
// =============================================================================

TEST_CASE("MobileAdapter::pinch establishes baseline then maps delta to wheel") {
    EventProbeGuard g;

    MobileAdapter ma;
    ma.onPinch(100.0f, 100.0f, 1.0f);          // baseline, no event
    CHECK(g.probe.count == 0);
    CHECK(ma.getLastPinchScale() == doctest::Approx(1.0f));

    ma.onPinch(100.0f, 100.0f, 1.25f);         // +0.25 → wheel y=+25
    REQUIRE(g.probe.count == 1);
    CHECK(g.probe.last.type == SDL_EVENT_MOUSE_WHEEL);
    CHECK(g.probe.last.wheel.y == doctest::Approx(25.0f));
    CHECK(ma.getLastPinchScale() == doctest::Approx(1.25f));

    ma.onPinch(100.0f, 100.0f, 1.0f);          // zoom back out: -0.25
    REQUIRE(g.probe.count == 2);
    CHECK(g.probe.last.wheel.y == doctest::Approx(-25.0f));

    ma.onPinch(100.0f, 100.0f, 1.0f);          // no delta: no event
    CHECK(g.probe.count == 2);
}

TEST_CASE("MobileAdapter::resetPinch ends gesture") {
    EventProbeGuard g;

    MobileAdapter ma;
    ma.onPinch(100.0f, 100.0f, 1.0f);
    CHECK(ma.getLastPinchScale() == doctest::Approx(1.0f));

    ma.resetPinch();
    CHECK(ma.getLastPinchScale() == 0.0f);

    ma.onPinch(100.0f, 100.0f, 2.0f);          // new gesture: baseline only
    CHECK(g.probe.count == 0);
    CHECK(ma.getLastPinchScale() == doctest::Approx(2.0f));
}

TEST_CASE("MobileAdapter::non-finite inputs are rejected") {
    EventProbeGuard g;

    MobileAdapter ma;
    ma.onFingerDown(NAN, 10.0f, 0);            // NaN x: ignored
    ma.onFingerDown(10.0f, INFINITY, 1);       // Inf y: ignored
    CHECK(ma.activeTouchCount() == 0);
    CHECK(g.probe.count == 0);

    ma.onPinch(100.0f, 100.0f, NAN);           // NaN scale: no baseline poison
    CHECK(ma.getLastPinchScale() == 0.0f);
    ma.onPinch(100.0f, 100.0f, 1.5f);          // still a fresh baseline
    CHECK(g.probe.count == 0);
    CHECK(ma.getLastPinchScale() == doctest::Approx(1.5f));

    ma.onFingerDown(10.0f, 10.0f, 0);
    CHECK(ma.activeTouchCount() == 1);
    ma.onFingerMotion(NAN, 10.0f, 0);          // NaN motion: ignored
    CHECK(g.probe.count == 1);                 // only the valid down event
    ma.onFingerUp(10.0f, NAN, 0);              // NaN up: state unchanged
    CHECK(ma.activeTouchCount() == 1);
    CHECK(ma.isFingerDown(0));

    ma.onLongPress(INFINITY, 10.0f);           // Inf long press: ignored
    CHECK(g.probe.count == 1);

    ma.setDisplayScale(NAN);                   // NaN scale rejected -> 1.0
    CHECK(ma.getDisplayScale() == doctest::Approx(1.0f));
}

// ---- Orientation change (P7) ----------------------------------------------

TEST_CASE("MobileAdapter onOrientationChanged invokes Lua callback") {
    lua_State* L = luaL_newstate();
    REQUIRE(L);
    luaL_openlibs(L);
    // Register a Lua recorder.
    REQUIRE(luaL_dostring(L, "received = nil; function _G.onOrientationChanged(o) received = o end") == LUA_OK);

    MobileAdapter adapter;
    adapter.onOrientationChanged(L, "portrait");
    lua_getglobal(L, "received");
    REQUIRE(lua_isstring(L, -1));
    CHECK(std::string(lua_tostring(L, -1)) == "portrait");
    lua_pop(L, 1);

    // Unknown orientation strings pass through; null state is a no-op.
    adapter.onOrientationChanged(nullptr, "landscape");
    adapter.onOrientationChanged(L, "face_up");
    lua_getglobal(L, "received");
    CHECK(std::string(lua_tostring(L, -1)) == "face_up");
    lua_pop(L, 1);

    lua_close(L);
}


TEST_CASE("MobileAdapter::unified lifecycle low-memory / terminate (Track P2)") {
    MobileAdapter ma;
    // Without a Lua state both events are safe no-ops (null-safe).
    CHECK_NOTHROW(ma.onLowMemory(nullptr));
    CHECK_NOTHROW(ma.onTerminate(nullptr));
}

TEST_CASE("MobileAdapter::unified lifecycle invokes _G callbacks with balanced stack") {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    MobileAdapter ma;
    int calls = 0;
    lua_pushlightuserdata(L, &calls);
    lua_pushcclosure(L, [](lua_State* LL) -> int {
        int* c = static_cast<int*>(lua_touserdata(LL, lua_upvalueindex(1)));
        (*c)++;
        return 0;
    }, 1);
    lua_setglobal(L, "onLowMemory");
    lua_pushlightuserdata(L, &calls);
    lua_pushcclosure(L, [](lua_State* LL) -> int {
        int* c = static_cast<int*>(lua_touserdata(LL, lua_upvalueindex(1)));
        (*c)++;
        return 0;
    }, 1);
    lua_setglobal(L, "onTerminate");
    const int before = lua_gettop(L);
    ma.onLowMemory(L);
    CHECK(calls == 1);
    ma.onTerminate(L);
    CHECK(calls == 2);
    CHECK(lua_gettop(L) == before); // stack balanced
    lua_close(L);
}

TEST_CASE("MobileAdapter::unified lifecycle swallows callback errors safely") {
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    MobileAdapter ma;
    lua_pushcfunction(L, [](lua_State* LL) -> int { return luaL_error(LL, "boom"); });
    lua_setglobal(L, "onLowMemory");
    const int before = lua_gettop(L);
    CHECK_NOTHROW(ma.onLowMemory(L));
    CHECK(lua_gettop(L) == before);
    lua_close(L);
}

// Actual SDL queue for the legacy/default route; synchronous copied events for
// the new sink route. The capability adapters only let the unchanged legacy
// implementation compile for a real behavioral RED run.

namespace {
using U17TouchSink = std::function<void(const SDL_Event&)>;
template<class T> constexpr bool u17HasTouchSink = requires(T& a, U17TouchSink sink) { a.setEventSink(sink); };
template<class T> constexpr bool u17HasDeferredTouches = requires(T& a) { a.setDeferredTouchClicks(true); };
template<class T> constexpr bool u17HasCancelTouches = requires(T& a) { a.cancelTouches(); };

template<class T> bool u17InstallTouchSink(T& adapter, U17TouchSink sink) {
    if constexpr (u17HasTouchSink<T>) { adapter.setEventSink(std::move(sink)); return true; }
    return false;
}
template<class T> void u17SetDeferred(T& adapter, bool enabled) {
    if constexpr (u17HasDeferredTouches<T>) adapter.setDeferredTouchClicks(enabled);
}
template<class T> void u17Cancel(T& adapter) {
    if constexpr (u17HasCancelTouches<T>) adapter.cancelTouches();
}

struct U17TouchEvents {
    U17TouchEvents() {
        REQUIRE(SDL_InitSubSystem(SDL_INIT_EVENTS));
        clear();
    }
    ~U17TouchEvents() { clear(); SDL_QuitSubSystem(SDL_INIT_EVENTS); }
    static void clear() {
        // No pointer-bearing async/user events are discarded.
        SDL_FlushEvents(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_WHEEL);
        SDL_FlushEvents(SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP);
    }
};

std::vector<SDL_Event> u17TakeQueuedTouchInput() {
    std::vector<SDL_Event> result;
    std::array<SDL_Event, 64> events{};
    for (const auto range : {std::pair{SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_WHEEL},
                             std::pair{SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP}}) {
        const int count = SDL_PeepEvents(events.data(), int(events.size()), SDL_GETEVENT,
                                         range.first, range.second);
        REQUIRE(count >= 0);
        result.insert(result.end(), events.begin(), events.begin() + count);
    }
    return result;
}

bool u17IsButton(const SDL_Event& event, Uint32 type, Uint8 button) {
    return event.type == type && event.button.button == button;
}
size_t u17ButtonCount(const std::vector<SDL_Event>& events, Uint32 type, Uint8 button) {
    return size_t(std::count_if(events.begin(), events.end(), [&](const SDL_Event& event) {
        return u17IsButton(event, type, button);
    }));
}

struct U17CapturedTouch {
    U17TouchEvents eventSubsystem;
    MobileAdapter adapter;
    std::vector<SDL_Event> events;
    U17TouchSink afterEvent;
    bool directSink = false;
    U17CapturedTouch() {
        directSink = u17InstallTouchSink(adapter, [this](const SDL_Event& event) { observe(event); });
        if (!directSink) SDL_AddEventWatch(watchLegacy, this);
    }
    ~U17CapturedTouch() { if (!directSink) SDL_RemoveEventWatch(watchLegacy, this); }
    static bool SDLCALL watchLegacy(void* data, SDL_Event* event) {
        static_cast<U17CapturedTouch*>(data)->observe(*event);
        return true;
    }
    void observe(const SDL_Event& event) {
        if ((event.type >= SDL_EVENT_MOUSE_MOTION && event.type <= SDL_EVENT_MOUSE_WHEEL)
            || event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
            events.push_back(event);
            const auto callback = afterEvent;
            if (callback) callback(event);
        }
    }
};

void u17CheckTapPair(const std::vector<SDL_Event>& events, size_t first, float x, float y) {
    REQUIRE(events.size() >= first + 2);
    CHECK(u17IsButton(events[first], SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT));
    CHECK(events[first].button.down);
    CHECK(events[first].button.x == doctest::Approx(x));
    CHECK(events[first].button.y == doctest::Approx(y));
    CHECK(u17IsButton(events[first + 1], SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT));
    CHECK_FALSE(events[first + 1].button.down);
    CHECK(events[first + 1].button.x == doctest::Approx(x));
    CHECK(events[first + 1].button.y == doctest::Approx(y));
}
} // namespace

TEST_CASE("U17 MobileAdapter: concrete classification API is available") {
    CHECK(u17HasTouchSink<MobileAdapter>);
    CHECK(u17HasDeferredTouches<MobileAdapter>);
    CHECK(u17HasCancelTouches<MobileAdapter>);
}

TEST_CASE("U17 MobileAdapter: default route remains immediate in actual SDL queue") {
    U17TouchEvents eventSubsystem;
    MobileAdapter adapter;
    adapter.setDisplayScale(2);
    adapter.onFingerDown(10, 20, 0);
    adapter.onFingerMotion(12, 23, 0);
    adapter.onFingerUp(12, 23, 0);
    const auto events = u17TakeQueuedTouchInput();
    REQUIRE(events.size() == 3);
    CHECK(u17IsButton(events[0], SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT));
    CHECK(events[0].button.x == doctest::Approx(20));
    CHECK(events[0].button.y == doctest::Approx(40));
    CHECK(events[1].type == SDL_EVENT_MOUSE_MOTION);
    CHECK(events[1].motion.x == doctest::Approx(24));
    CHECK(events[1].motion.y == doctest::Approx(46));
    CHECK((events[1].motion.state & SDL_BUTTON_LMASK) != 0);
    CHECK(u17IsButton(events[2], SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT));
    CHECK_FALSE(events[2].button.down);
    CHECK(adapter.activeTouchCount() == 0);
}

TEST_CASE("U17 MobileAdapter: deferred small tap emits one release-position pair through sink") {
    U17CapturedTouch capture;
    auto& adapter = capture.adapter;
    u17SetDeferred(adapter, true);
    adapter.setDisplayScale(2);
    adapter.onFingerDown(10, 20, 0);
    CHECK(capture.events.empty());
    CHECK(adapter.activeTouchCount() == 1);
    adapter.onFingerMotion(12, 23, 0);
    REQUIRE(capture.events.size() == 1);
    CHECK(capture.events[0].type == SDL_EVENT_MOUSE_MOTION);
    CHECK((capture.events[0].motion.state & SDL_BUTTON_LMASK) == 0);
    bool releaseStatePublished = false;
    capture.afterEvent = [&](const SDL_Event& event) {
        if (u17IsButton(event, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT))
            releaseStatePublished = adapter.activeTouchCount() == 0 && !adapter.isFingerDown(0);
    };
    adapter.onFingerUp(12, 23, 0);
    REQUIRE(capture.events.size() == 3);
    u17CheckTapPair(capture.events, 1, 24, 46);
    CHECK(releaseStatePublished);
    adapter.onFingerUp(12, 23, 0); // A duplicate terminal event cannot click twice.
    CHECK(capture.events.size() == 3);
    CHECK(u17TakeQueuedTouchInput().empty()); // A configured sink must not also enqueue.
}

TEST_CASE("U17 MobileAdapter: displacement includes final up and maximum travel with existing 16px slop") {
    for (int variation = 0; variation != 4; ++variation) {
        CAPTURE(variation);
        U17CapturedTouch capture;
        auto& adapter = capture.adapter;
        u17SetDeferred(adapter, true);
        adapter.onFingerDown(20, 20, 0);
        if (variation == 0) adapter.onFingerUp(36, 20, 0); // Exactly 16: positive boundary.
        if (variation == 1) adapter.onFingerUp(37, 20, 0); // Final coordinates alone cross slop.
        if (variation == 2) {
            adapter.onFingerMotion(37, 20, 0);
            adapter.onFingerMotion(20, 20, 0);
            adapter.onFingerUp(20, 20, 0); // Returning to origin does not restore tap eligibility.
        }
        if (variation == 3) {
            adapter.onFingerDown(37, 20, 0); // Duplicate down updates position, not origin/count.
            CHECK(adapter.activeTouchCount() == 1);
            adapter.onFingerUp(20, 20, 0);
        }
        CHECK(u17ButtonCount(capture.events, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT)
            == size_t(variation == 0 ? 1 : 0));
        CHECK(adapter.activeTouchCount() == 0);
        capture.events.clear();
        adapter.onFingerDown(5, 6, 7);
        u17SetDeferred(adapter, true); // Same mode must not cancel this new contact.
        adapter.onFingerUp(5, 6, 7);
        REQUIRE(capture.events.size() == 2);
        u17CheckTapPair(capture.events, 0, 5, 6);
    }
}

TEST_CASE("U17 MobileAdapter: multiple contacts suppress the complete sequence then permit a fresh tap") {
    U17CapturedTouch capture;
    auto& adapter = capture.adapter;
    u17SetDeferred(adapter, true);
    adapter.onFingerDown(10, 10, 0);
    adapter.onFingerDown(20, 20, 1);
    CHECK(adapter.activeTouchCount() == 2);
    adapter.onFingerUp(20, 20, 1);
    CHECK(adapter.activeTouchCount() == 1);
    adapter.onFingerUp(10, 10, 0);
    CHECK(capture.events.empty());
    CHECK(adapter.activeTouchCount() == 0);
    adapter.onFingerDown(7, 8, 7);
    adapter.onFingerUp(7, 8, 7);
    REQUIRE(capture.events.size() == 2);
    u17CheckTapPair(capture.events, 0, 7, 8);
}

TEST_CASE("U17 MobileAdapter: recognized gestures retain their mapping and consume ordinary deferred tap") {
    for (int gesture = 0; gesture != 6; ++gesture) {
        CAPTURE(gesture);
        U17CapturedTouch capture;
        auto& adapter = capture.adapter;
        u17SetDeferred(adapter, true);
        adapter.onFingerDown(50, 60, 0);
        if (gesture == 0) adapter.onLongPress(50, 60);
        if (gesture == 1) { adapter.onPinch(50, 60, 1); adapter.onPinch(50, 60, 1.25f); }
        if (gesture == 2) adapter.onTwoFingerTap(50, 60);
        if (gesture == 3) adapter.onThreeFingerHold(50, 60);
        if (gesture == 4) adapter.onSwipeDown(50, 60, 50, 140);
        if (gesture == 5) adapter.onSwipeUp(50, 60, 50, -20);
        adapter.onFingerUp(50, 60, 0);
        CHECK(u17ButtonCount(capture.events, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT) == 0);
        if (gesture == 0 || gesture == 2) {
            CHECK(u17ButtonCount(capture.events, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_RIGHT) == 1);
            CHECK(u17ButtonCount(capture.events, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_RIGHT) == 1);
        } else {
            REQUIRE(capture.events.size() == 1);
            if (gesture == 1) {
                CHECK(capture.events[0].type == SDL_EVENT_MOUSE_WHEEL);
                CHECK(capture.events[0].wheel.y == doctest::Approx(25));
            } else {
                CHECK(capture.events[0].type == SDL_EVENT_KEY_DOWN);
                CHECK(capture.events[0].key.key == (gesture == 3 ? SDLK_LCTRL
                    : gesture == 4 ? SDLK_SPACE : SDLK_PAGEUP));
            }
        }
        CHECK(u17TakeQueuedTouchInput().empty());
        capture.events.clear();
        adapter.onFingerDown(3, 4, 0);
        adapter.onFingerUp(3, 4, 0);
        REQUIRE(capture.events.size() == 2);
        u17CheckTapPair(capture.events, 0, 3, 4);
    }
}

TEST_CASE("U17 MobileAdapter: cancel and mode changes clear contacts and release an emitted left button") {
    U17CapturedTouch capture;
    auto& adapter = capture.adapter;
    adapter.onFingerDown(10, 20, 0); // Compatibility mode holds a synthetic left button.
    adapter.onFingerMotion(12, 23, 0);
    bool clearedBeforeRelease = false;
    capture.afterEvent = [&](const SDL_Event& event) {
        if (u17IsButton(event, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT))
            clearedBeforeRelease = adapter.activeTouchCount() == 0 && !adapter.isFingerDown(0);
    };
    u17SetDeferred(adapter, true);
    CHECK(clearedBeforeRelease);
    CHECK(adapter.activeTouchCount() == 0);
    CHECK(u17ButtonCount(capture.events, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT) == 1);
    const size_t previous = capture.events.size();
    adapter.onFingerUp(12, 23, 0);
    CHECK(capture.events.size() == previous);
    capture.afterEvent = {};
    capture.events.clear();
    adapter.onFingerDown(30, 40, 0);
    adapter.onPinch(30, 40, 1);
    u17Cancel(adapter);
    CHECK(adapter.activeTouchCount() == 0);
    CHECK_FALSE(adapter.isFingerDown(0));
    CHECK(adapter.getLastPinchScale() == 0);
    adapter.onFingerUp(30, 40, 0);
    CHECK(capture.events.empty());
    adapter.onFingerDown(5, 6, 1);
    u17SetDeferred(adapter, false);
    CHECK(adapter.activeTouchCount() == 0);
    adapter.onFingerUp(5, 6, 1);
    CHECK(capture.events.empty());
    adapter.onFingerDown(80, 90, 7);
    adapter.onFingerMotion(81, 91, 7);
    u17Cancel(adapter);
    REQUIRE(capture.events.size() == 3);
    CHECK((capture.events[1].motion.state & SDL_BUTTON_LMASK) != 0);
    CHECK(u17IsButton(capture.events[2], SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT));
    u17Cancel(adapter);
    CHECK(capture.events.size() == 3);
}

TEST_CASE("U17 MobileAdapter: sink cancellation balances old down before a reentrant new immediate down") {
    for (const bool initiallyDeferred : {false, true}) {
        CAPTURE(initiallyDeferred);
        U17CapturedTouch capture;
        auto& adapter = capture.adapter;
        u17SetDeferred(adapter, initiallyDeferred);
        const int successorSlot = initiallyDeferred ? 0 : 1;
        bool reentered = false;
        capture.afterEvent = [&](const SDL_Event& event) {
            if (!reentered && u17IsButton(event, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT)) {
                reentered = true;
                u17Cancel(adapter); // Must emit old UP with old ownership already cleared.
                u17SetDeferred(adapter, false);
                adapter.onFingerDown(80, 90, successorSlot);
            }
        };
        adapter.onFingerDown(10, 20, 0);
        adapter.onFingerUp(10, 20, 0);
        REQUIRE(capture.events.size() == 3);
        CHECK(u17IsButton(capture.events[0], SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT));
        CHECK(u17IsButton(capture.events[1], SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT));
        CHECK(u17IsButton(capture.events[2], SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT));
        CHECK(capture.events[2].button.x == doctest::Approx(80));
        CHECK(adapter.activeTouchCount() == 1);
        CHECK(adapter.isFingerDown(successorSlot));
        if (successorSlot != 0) CHECK_FALSE(adapter.isFingerDown(0));
        adapter.onFingerMotion(81, 91, successorSlot);
        REQUIRE(capture.events.size() == 4);
        CHECK(capture.events[3].type == SDL_EVENT_MOUSE_MOTION);
        CHECK((capture.events[3].motion.state & SDL_BUTTON_LMASK) != 0);
        adapter.onFingerUp(81, 91, successorSlot);
        REQUIRE(capture.events.size() == 5);
        CHECK(u17IsButton(capture.events[4], SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT));
        CHECK(adapter.activeTouchCount() == 0);
    }
}

TEST_CASE("U17 MobileAdapter: reentrant new deferred contact survives old tap's balancing release") {
    U17CapturedTouch capture;
    auto& adapter = capture.adapter;
    u17SetDeferred(adapter, true);
    bool reentered = false;
    capture.afterEvent = [&](const SDL_Event& event) {
        if (!reentered && u17IsButton(event, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT)) {
            reentered = true;
            adapter.onFingerDown(80, 90, 0); // Reuse released slot; no cancel or new emitted left-down.
        }
    };
    adapter.onFingerDown(10, 20, 0);
    adapter.onFingerUp(10, 20, 0);
    REQUIRE(capture.events.size() == 2);
    u17CheckTapPair(capture.events, 0, 10, 20);
    CHECK(adapter.activeTouchCount() == 1);
    CHECK(adapter.isFingerDown(0));
    adapter.onFingerMotion(81, 91, 0);
    REQUIRE(capture.events.size() == 3);
    CHECK((capture.events[2].motion.state & SDL_BUTTON_LMASK) == 0);
    adapter.onFingerUp(81, 91, 0);
    REQUIRE(capture.events.size() == 5);
    u17CheckTapPair(capture.events, 3, 81, 91);
    CHECK(adapter.activeTouchCount() == 0);
}
