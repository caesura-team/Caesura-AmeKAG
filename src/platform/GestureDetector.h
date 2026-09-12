#pragma once

#include <cstdint>

namespace Caesura {

// GestureDetector — raw finger-stream (SDL touch) → discrete gesture events.
//
// Pure platform-layer state machine (no SDL dependency, no rendering): the
// Engine feeds FINGER_DOWN/MOTION/UP coordinates in WINDOW PIXELS plus a
// millisecond clock (SDL_GetTicks), then polls tick() once per frame. It
// emits:
//
//   LongPress       — one finger held still >= 500 ms (MobileAdapter::onLongPress
//                     maps it to a right-click; the platform layer owns the timer).
//   Pinch           — two-finger distance ratio changes past a slop, emitted as
//                     incremental ratio pulses (MobileAdapter::onPinch).
//   TwoFingerTap    — two fingers tapped and released within 300 ms (travel <= 20 px).
//   ThreeFingerHold — three fingers held still >= 200 ms (travel <= 25 px).
//   SwipeDown       — single finger vertical downward swipe (dy >= 50 px, |dy| >= 1.5|dx|).
//   SwipeUp         — single finger vertical upward swipe (dy <= -50 px, |dy| >= 1.5|dx|).
//
// Coords stay in the input domain (window px); the adapter applies its own
// display scale. Engine maps native identities into these eight slots; the
// standalone MobileAdapter retains its separate capacity of ten contacts.
//
// ── Two-端语义对齐 (C6 native ⇄ C7 web) ────────────────────────────────────
// web/touch-gestures.js implements the SAME four gestures for the DOM player.
// The numeric thresholds below are now identical to its defaults, so a gesture
// that fires on a phone browser also fires in the native build:
//
//   gesture           | native (here)             | web (touch-gestures.js)
//   ------------------+---------------------------+--------------------------------
//   TwoFingerTap      | <= 300 ms, travel <= 20px | twoFingerTapMaxDurationMs 300,
//                     |                           | twoFingerTapMaxMovePx 20
//   ThreeFingerHold   | >= 200 ms, travel <= 25px | threeFingerHoldMinDurationMs 200,
//                     |                           | threeFingerHoldMaxMovePx 25
//   Swipe up/down     | >= 50 px, |dy| >= 1.5|dx| | swipeMinDistancePx 50,
//                     |                           | swipeDirectionRatio 1.5
//
// TWO DIVERGENCES REMAIN, both deliberate:
//
//  1. ThreeFingerHold is a TOGGLE natively and MOMENTARY on the web. The web
//     detector calls onThreeFingerHold({active:true}) on hold and
//     ({active:false}) on release, and main.mjs writes skipMode = active, i.e.
//     skip lasts exactly as long as the fingers stay down. Natively the event
//     is a single pulse that MobileAdapter maps to an LCTRL keydown, which
//     Engine.cpp forwards to _KAG_onCtrlDown — the SAME toggle a desktop Ctrl
//     press performs. Matching the desktop keyboard contract is worth more than
//     matching the web here: the native build shares its skip handler with the
//     keyboard, and a momentary variant would need a hold-RELEASE event plus a
//     new IMobileAdapter method and Engine dispatch (Engine.cpp is outside this
//     task's file set). Recorded as a known, intentional difference.
//  2. Native tracks the whole touch SEQUENCE: once a second finger has touched
//     down, lifting the last finger can no longer be read as a single-finger
//     swipe (see m_seqMaxFingers). The web detector gets this for free because
//     it only inspects gestures when this.touches.size hits 0 with exactly one
//     ending touch; the native stream is per-finger, so the guard is explicit.

struct GestureEvent {
    enum class Kind {
        None,
        LongPress,
        Pinch,
        TwoFingerTap,
        ThreeFingerHold,
        SwipeDown,
        SwipeUp
    };
    Kind  kind = Kind::None;
    float x = 0.0f;        // LongPress / TwoFingerTap / ThreeFingerHold: centroid x; Swipe: end x
    float y = 0.0f;        // Centroid y; Swipe: end y
    float scale = 1.0f;    // Pinch: current distance / baseline distance
    float deltaX = 0.0f;   // Swipe: total delta x
    float deltaY = 0.0f;   // Swipe: total delta y
};

class GestureDetector {
public:
    static constexpr double kLongPressMs       = 500.0;
    static constexpr double kTwoFingerTapMaxMs = 300.0;
    static constexpr double kThreeFingerHoldMs = 200.0;
    // kMoveSlop stays 16 px: it is the pre-existing long-press/drag threshold
    // and changing it would alter shipped single-finger behavior. The two
    // multi-finger gestures carry their own tolerances, matching the web
    // detector's twoFingerTapMaxMovePx / threeFingerHoldMaxMovePx (a hand
    // resting three fingers on glass wobbles more than one finger does).
    static constexpr float  kMoveSlop          = 16.0f;   // px before a "hold" becomes a drag
    static constexpr float  kTwoFingerTapSlop  = 20.0f;   // == web twoFingerTapMaxMovePx
    static constexpr float  kThreeFingerHoldSlop = 25.0f; // == web threeFingerHoldMaxMovePx
    static constexpr float  kPinchInitial      = 0.08f;   // first ratio change that starts a pinch
    static constexpr float  kPinchStep         = 0.02f;   // later incremental pulse threshold
    static constexpr float  kSwipeMinDistance  = 50.0f;   // == web swipeMinDistancePx
    static constexpr float  kSwipeDominanceRatio = 1.5f;  // == web swipeDirectionRatio
    static constexpr int    kMaxFingers        = 8;
    static constexpr int    kMaxPendingEvents  = 8;

    void reset();

    void onFingerDown(int id, float x, float y, double nowMs);
    void onFingerMove(int id, float x, float y, double nowMs);
    void onFingerUp(int id, double nowMs);

    // Poll for at most ONE gesture event
    GestureEvent tick(double nowMs);

    int activeFingerCount() const;

private:
    struct Finger {
        bool    active = false;
        int     id = -1;
        float   x = 0.0f, y = 0.0f;
        float   startX = 0.0f, startY = 0.0f;
        // Largest squared distance this finger has been from its start point.
        // A wobble that returns to the origin must still disqualify a tap, so
        // the maximum is kept rather than the current offset (same idea as the
        // web detector's per-touch maxTravel).
        float   maxTravelSq = 0.0f;
        double  downAtMs = 0.0;
        bool    moved = false;
    };

    void beginPinchBase(double nowMs);
    Finger* findFinger(int id);
    void pushPendingEvent(const GestureEvent& ev);
    float maxActiveTravel() const;

    Finger m_fingers[kMaxFingers];
    int    m_active = 0;
    bool   m_longPressFired = false;
    float  m_pinchBaseDist = 0.0f;
    float  m_pinchLastRatio = 1.0f;
    bool   m_pinchStarted = false;

    // Two-finger tap state
    bool   m_twoFingerActive = false;
    double m_twoFingerDownMs = 0.0;
    bool   m_twoFingerMoved = false;

    // Three-finger hold state
    bool   m_threeFingerActive = false;
    double m_threeFingerDownMs = 0.0;
    bool   m_threeFingerHoldFired = false;

    // Highest finger count seen since the last time every finger was up. A
    // two-finger drag ends with a single remaining finger, and without this the
    // final lift would be mistaken for a one-finger swipe (web gets this free
    // by only looking at touches.size == 0 with one ending touch).
    int    m_seqMaxFingers = 0;

    // Pending event queue for discrete events recognized on finger up
    GestureEvent m_pendingEvents[kMaxPendingEvents];
    int m_pendingHead = 0;
    int m_pendingTail = 0;
};

} // namespace Caesura
