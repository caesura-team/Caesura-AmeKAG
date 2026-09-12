#include "GestureDetector.h"
#include <cmath>

namespace Caesura {

void GestureDetector::reset() {
    for (auto& f : m_fingers) { f = Finger{}; }
    m_active = 0;
    m_longPressFired = false;
    m_pinchBaseDist = 0.0f;
    m_pinchLastRatio = 1.0f;
    m_pinchStarted = false;
    m_twoFingerActive = false;
    m_twoFingerDownMs = 0.0;
    m_twoFingerMoved = false;
    m_threeFingerActive = false;
    m_threeFingerDownMs = 0.0;
    m_threeFingerHoldFired = false;
    m_seqMaxFingers = 0;
    m_pendingHead = 0;
    m_pendingTail = 0;
}

GestureDetector::Finger* GestureDetector::findFinger(int id) {
    for (auto& f : m_fingers) {
        if (f.active && f.id == id) return &f;
    }
    return nullptr;
}

void GestureDetector::pushPendingEvent(const GestureEvent& ev) {
    const int nextTail = (m_pendingTail + 1) % kMaxPendingEvents;
    if (nextTail != m_pendingHead) {
        m_pendingEvents[m_pendingTail] = ev;
        m_pendingTail = nextTail;
    }
}

void GestureDetector::beginPinchBase(double /*nowMs*/) {
    float ax = 0, ay = 0, bx = 0, by = 0;
    int n = 0;
    for (const auto& f : m_fingers) {
        if (!f.active) continue;
        if (n == 0) { ax = f.x; ay = f.y; }
        else if (n == 1) { bx = f.x; by = f.y; }
        ++n;
    }
    if (n >= 2 && std::hypot(bx - ax, by - ay) > 0.0f) {
        m_pinchBaseDist = std::hypot(bx - ax, by - ay);
        m_pinchLastRatio = 1.0f;
        m_pinchStarted = false;
    } else {
        m_pinchBaseDist = 0.0f;
    }
}

void GestureDetector::onFingerDown(int id, float x, float y, double nowMs) {
    if (findFinger(id)) return;  // duplicate down: ignore
    for (auto& f : m_fingers) {
        if (!f.active) {
            f = Finger{};
            f.active = true;
            f.id = id;
            f.x = x; f.y = y;
            f.startX = x; f.startY = y;
            f.downAtMs = nowMs;
            f.moved = false;
            ++m_active;
            break;
        }
    }
    // A new finger invalidates an in-flight long-press and re-bases a pinch.
    m_longPressFired = false;

    if (m_active > m_seqMaxFingers) m_seqMaxFingers = m_active;

    if (m_active == 2) {
        m_twoFingerActive = true;
        m_twoFingerDownMs = nowMs;
        m_twoFingerMoved = false;
    } else {
        m_twoFingerActive = false;
    }

    if (m_active == 3) {
        m_threeFingerActive = true;
        m_threeFingerDownMs = nowMs;
        m_threeFingerHoldFired = false;
    } else {
        m_threeFingerActive = false;
    }

    beginPinchBase(nowMs);
}

void GestureDetector::onFingerMove(int id, float x, float y, double /*nowMs*/) {
    Finger* f = findFinger(id);
    if (!f) return;
    const float dx = x - f->startX, dy = y - f->startY;
    const float travelSq = dx * dx + dy * dy;
    // Remember the LARGEST travel this finger has shown, not the latest: a
    // finger that wanders 40 px and comes back to its origin is not a tap, and
    // comparing only the current offset would call it one (this mirrors the web
    // detector's per-touch maxTravel bookkeeping).
    if (travelSq > f->maxTravelSq) f->maxTravelSq = travelSq;
    // moved == "no longer a still hold" for the single-finger long-press path,
    // which keeps its historical 16 px slop. Multi-finger gestures compare
    // maxTravelSq against their own, looser tolerances at recognition time.
    if (travelSq > kMoveSlop * kMoveSlop) f->moved = true;
    f->x = x; f->y = y;
}

// Largest travel across the fingers currently down (px). Used by the tap/hold
// recognizers, which tolerate more wobble than the long-press slop.
float GestureDetector::maxActiveTravel() const {
    float worstSq = 0.0f;
    for (const auto& f : m_fingers) {
        if (f.active && f.maxTravelSq > worstSq) worstSq = f.maxTravelSq;
    }
    return std::sqrt(worstSq);
}

void GestureDetector::onFingerUp(int id, double nowMs) {
    Finger* f = findFinger(id);
    if (f && f->active) {
        // 1. Single-finger vertical swipe. m_seqMaxFingers == 1 keeps a
        // multi-finger gesture from decaying into a swipe: lifting the last of
        // two fingers leaves m_active == 1, and without the sequence guard that
        // final lift would be read as a one-finger drag.
        if (m_active == 1 && m_seqMaxFingers == 1) {
            const float dx = f->x - f->startX;
            const float dy = f->y - f->startY;
            const float absDx = std::fabs(dx);
            const float absDy = std::fabs(dy);

            if (absDy >= kSwipeMinDistance && absDy >= absDx * kSwipeDominanceRatio) {
                if (dy > 0.0f) {
                    pushPendingEvent({ GestureEvent::Kind::SwipeDown, f->x, f->y, 1.0f, dx, dy });
                } else {
                    pushPendingEvent({ GestureEvent::Kind::SwipeUp, f->x, f->y, 1.0f, dx, dy });
                }
            }
        }

        // 2. Two-finger tap: <= 300 ms and travel within the tap tolerance
        // (20 px, matching web twoFingerTapMaxMovePx). m_seqMaxFingers == 2
        // rejects "three fingers down, one lifted" from counting as a tap.
        if (m_twoFingerActive && m_active == 2 && m_seqMaxFingers == 2 &&
            maxActiveTravel() <= kTwoFingerTapSlop) {
            const double duration = nowMs - m_twoFingerDownMs;
            if (duration <= kTwoFingerTapMaxMs) {
                float cx = 0.0f, cy = 0.0f;
                int count = 0;
                for (const auto& finger : m_fingers) {
                    if (finger.active) {
                        cx += finger.x;
                        cy += finger.y;
                        ++count;
                    }
                }
                if (count > 0) {
                    pushPendingEvent({ GestureEvent::Kind::TwoFingerTap, cx / count, cy / count, 1.0f, 0.0f, 0.0f });
                }
            }
            m_twoFingerActive = false;
        }

        // 3. Three-finger hold cancellation / release
        if (m_threeFingerActive) {
            m_threeFingerActive = false;
        }

        *f = Finger{};
        --m_active;
    }
    // Gesture boundary: long-press candidate gone, pinch base gone.
    m_longPressFired = false;
    // Every finger up ends the touch SEQUENCE: the next contact starts fresh
    // and may legitimately be a one-finger swipe again.
    if (m_active == 0) m_seqMaxFingers = 0;
    beginPinchBase(nowMs);
}

GestureEvent GestureDetector::tick(double nowMs) {
    // 1. Check pending event queue (e.g. Swipes, TwoFingerTap)
    if (m_pendingHead != m_pendingTail) {
        GestureEvent ev = m_pendingEvents[m_pendingHead];
        m_pendingHead = (m_pendingHead + 1) % kMaxPendingEvents;
        return ev;
    }

    // 2. Long press: one finger, held still, past the timeout.
    if (m_active == 1 && m_seqMaxFingers == 1 && !m_longPressFired) {
        for (const auto& f : m_fingers) {
            if (!f.active) continue;
            if (!f.moved && (nowMs - f.downAtMs) >= kLongPressMs) {
                m_longPressFired = true;
                return { GestureEvent::Kind::LongPress, f.x, f.y, 1.0f, 0.0f, 0.0f };
            }
            break;
        }
    }

    // 3. Three fingers held still >= kThreeFingerHoldMs, travel within the
    // hold tolerance (25 px, matching web threeFingerHoldMaxMovePx — a hand
    // resting three fingers on glass wobbles more than one finger does, which
    // is why this is looser than the 16 px long-press slop).
    if (m_active == 3 && m_threeFingerActive && !m_threeFingerHoldFired) {
        float cx = 0.0f, cy = 0.0f;
        for (const auto& f : m_fingers) {
            if (!f.active) continue;
            cx += f.x;
            cy += f.y;
        }
        const bool steady = maxActiveTravel() <= kThreeFingerHoldSlop;
        if (steady && (nowMs - m_threeFingerDownMs) >= kThreeFingerHoldMs) {
            m_threeFingerHoldFired = true;
            return { GestureEvent::Kind::ThreeFingerHold, cx / 3.0f, cy / 3.0f, 1.0f, 0.0f, 0.0f };
        }
    }

    // 4. Pinch: two or more fingers, distance ratio pulses.
    if (m_active >= 2 && m_pinchBaseDist > 0.0f) {
        float ax = 0, ay = 0, bx = 0, by = 0;
        int n = 0;
        for (const auto& f : m_fingers) {
            if (!f.active) continue;
            if (n == 0) { ax = f.x; ay = f.y; }
            else if (n == 1) { bx = f.x; by = f.y; }
            ++n;
        }
        if (n >= 2) {
            const float d = std::hypot(bx - ax, by - ay);
            if (d > 0.0f) {
                const float ratio = d / m_pinchBaseDist;
                const bool start = !m_pinchStarted && std::fabs(ratio - 1.0f) >= kPinchInitial;
                const bool step  = m_pinchStarted && std::fabs(ratio - m_pinchLastRatio) >= kPinchStep;
                if (start || step) {
                    m_pinchStarted = true;
                    m_pinchLastRatio = ratio;
                    return { GestureEvent::Kind::Pinch,
                             (ax + bx) * 0.5f, (ay + by) * 0.5f, ratio, 0.0f, 0.0f };
                }
            }
        }
    }
    return { GestureEvent::Kind::None, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
}

int GestureDetector::activeFingerCount() const { return m_active; }

} // namespace Caesura
