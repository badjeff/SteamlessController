#include "TrackpadInput.h"
#include "EventLog.h"
#include "InputInjection.h"
#include "steam/SteamController.h"
#include <Windows.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>

void TrackpadInput::SetMode(TrackpadMode mode) {
    if (mode == m_mode) return;
    m_mode = mode;
    Reset();  // carried remainders and touch state mean nothing to the new mode
}

void TrackpadInput::SetDiagonals(DiagonalMode d) {
    if (d == m_diagonals) return;
    m_diagonals = d;
    // The sector numbering means something different either side of this, so a
    // held one cannot carry over. Dropping it re-resolves against the new
    // geometry on the next frame; the directions it produced are released by
    // the same edge detection that handles any other change.
    m_sector = -1;
    m_dirs   = DirNone;
}

void TrackpadInput::Reset() {
    m_touching   = false;
    m_prevX      = 0;
    m_prevY      = 0;
    m_remX       = 0.0f;
    m_remY       = 0.0f;
    m_scrollRemX = 0.0f;
    m_scrollRemY = 0.0f;
    m_scrollCircularRem = 0.0f;
    m_haveScrollAngle   = false;
    m_haveRef    = false;
    m_travelSent = 0;

    StopGlide();
    m_velX           = 0.0f;
    m_velY           = 0.0f;
    m_haveVelocity   = false;
    m_contactPressed = false;
    m_haveUpdateTime = false;

    m_dirs           = DirNone;
    m_sector         = -1;
    m_clickHeld      = false;
    m_clickInCentre  = true;
}

// Tracks whether accepted movement is reaching the cursor. Re-arms whenever the
// cursor does move, so only a genuinely pinned cursor accumulates travel.
void TrackpadInput::NoteMovementSent(long px, bool haveCursor,
                                     long cursorX, long cursorY) {
    if (!haveCursor) {
        // No cursor position to compare against — GetCursorPos itself fails
        // off the input desktop, and InputInjection reports that separately.
        m_haveRef    = false;
        m_travelSent = 0;
        return;
    }

    if (!m_haveRef || cursorX != m_refCursorX || cursorY != m_refCursorY) {
        m_refCursorX = cursorX;
        m_refCursorY = cursorY;
        m_haveRef    = true;
        m_travelSent = 0;
    }

    m_travelSent += px;
    if (m_travelSent >= STUCK_TRAVEL_PX) {
        POINT at{ m_refCursorX, m_refCursorY };
        InputInjection::LogCursorNotMoving(m_travelSent, at);
        m_travelSent = 0;  // re-arm; the log call rate-limits itself
    }
}

// The pad's speed setting scales pointer movement, the same one scroll uses
// (#129): 100 is the built-in feel, so a pad that never touched the setting
// moves as it always did.
float TrackpadInput::PointerSensitivity() const {
    return SENSITIVITY * (static_cast<float>(m_scrollSpeed) / 100.0f);
}

// Pixel-space injection, shared by finger movement and the glide. Carry
// sub-pixel remainders between frames — per-frame deltas scaled by sensitivity
// are often below one pixel, and truncating them each frame would discard slow
// movement entirely.
void TrackpadInput::EmitPointer(float dxPx, float dyPx) {
    const float fx = dxPx + m_remX;
    const float fy = dyPx + m_remY;
    const LONG  ix = static_cast<LONG>(fx);
    const LONG  iy = static_cast<LONG>(fy);
    m_remX = fx - static_cast<float>(ix);
    m_remY = fy - static_cast<float>(iy);
    if (ix == 0 && iy == 0) return;

    INPUT input{};
    input.type       = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx      = ix;
    input.mi.dy      = iy;
    // Read the cursor before sending: SendInput queues the event rather than
    // applying it, so a position read straight after would still be the old one.
    POINT      before{};
    const bool haveBefore = GetCursorPos(&before) != FALSE;
    if (InputInjection::Send(input, "trackpad-move")) {
        NoteMovementSent(std::labs(ix) + std::labs(iy),
                         haveBefore, before.x, before.y);
    }
}

void TrackpadInput::UpdatePointer(int dx, int dy) {
    const float sensitivity = PointerSensitivity();
    EmitPointer(static_cast<float>(dx) * sensitivity,
                static_cast<float>(dy) * sensitivity);
}

// Folds this frame's finger movement into the velocity that a lift will hand
// to the glide. Called on every touched frame, including a still one, so a
// thumb that stops before lifting has already decayed to nothing by the time
// the contact ends.
void TrackpadInput::UpdateVelocity(float dt, float dxPx, float dyPx) {
    if (dt <= 0.0f) return;

    // A frame that moved further than a finger can is a sensor jump, or a
    // lift and re-placement whose touch bit we never saw go false — not a
    // flick. Letting it into the filter would launch the cursor across the
    // screen on the next lift.
    if (std::hypot(dxPx, dyPx) > JUMP_REPORT_UNITS * PointerSensitivity()) {
        m_velX = 0.0f;
        m_velY = 0.0f;
        m_haveVelocity = false;
        return;
    }

    const float instX = dxPx / dt;
    const float instY = dyPx / dt;
    if (!m_haveVelocity) {
        // Seed at the first moving frame's own velocity: a flick is often only
        // two or three reports long, and easing up from zero would discard the
        // speed. A still frame before any movement leaves it unseeded.
        if (dxPx == 0.0f && dyPx == 0.0f) return;
        m_velX = instX;
        m_velY = instY;
        m_haveVelocity = true;
    } else {
        // Time-based rather than per-frame, so the same flick measures the same
        // at 60 Hz and at 250 Hz.
        const float alpha = 1.0f - std::exp(-dt / GLIDE_VELOCITY_TAU_S);
        m_velX += (instX - m_velX) * alpha;
        m_velY += (instY - m_velY) * alpha;
    }

    // A ceiling, for the report that is merely fast rather than impossible.
    const float speed = std::hypot(m_velX, m_velY);
    if (speed > GLIDE_MAX_SPEED_PX) {
        const float scale = GLIDE_MAX_SPEED_PX / speed;
        m_velX *= scale;
        m_velY *= scale;
    }
}

void TrackpadInput::StartGlide() {
    if (!m_haveVelocity) return;

    const float speed = std::hypot(m_velX, m_velY);
    // Below the configured start speed the thumb was placing the cursor, not
    // throwing it. A glide from that would read as the cursor drifting after
    // every slow adjustment. A start speed of zero means coast on every
    // release that moved.
    if (speed < m_glideSpeedPx) {
        StopGlide();
        return;
    }

    m_glideVX       = m_velX;
    m_glideVY       = m_velY;
    m_glideElapsedS = 0.0f;
    m_glideActive   = true;
}

void TrackpadInput::UpdateGlide(float dt) {
    if (dt <= 0.0f) return;

    m_glideElapsedS += dt;
    if (m_glideElapsedS > GLIDE_MAX_DURATION_S) {
        StopGlide();
        return;
    }

    // The profile stores a percentage kept per GLIDE_DECAY_TICK_S. Raising it
    // to dt/tick turns that into the fraction for this report's actual
    // interval, so the coast does not depend on whether the pad is reporting
    // at 60 Hz or 250 Hz.
    const float decay = std::pow(m_glideDecay, dt / GLIDE_DECAY_TICK_S);
    m_glideVX *= decay;
    m_glideVY *= decay;

    const float speed = std::hypot(m_glideVX, m_glideVY);
    if (speed < GLIDE_STOP_SPEED_PX) {
        StopGlide();
        return;
    }

    EmitPointer(m_glideVX * dt, m_glideVY * dt);
}

void TrackpadInput::StopGlide() {
    m_glideActive   = false;
    m_glideVX       = 0.0f;
    m_glideVY       = 0.0f;
    m_glideElapsedS = 0.0f;
}

// The wheel units one frame of movement is worth.
//
// Two corrections sit on top of the built-in scale, and they answer different
// questions. The first is not a preference at all: Windows multiplies every
// notch we send by the machine's lines-per-notch setting before anything
// moves, so the same swipe scrolls three times as far on a machine set to 10
// as on one left at the default 3. Dividing it back out is what makes the pad
// behave the same everywhere, and it is why a scroll-is-far-too-fast report
// can be true on one machine and not reproduce on another with identical code.
//
// The second is the user's own scroll speed, which is taste and nothing else.
// It is applied after the correction so the percentage means the same thing on
// every machine: 100% is the calibrated feel, not "whatever this machine does".
float TrackpadInput::ScrollScale() const {
    const float correction =
        InputInjection::WheelCorrection(InputInjection::WheelLinesPerNotch());
    return SCROLL_SENSITIVITY * correction
         * (static_cast<float>(m_scrollSpeed) / 100.0f);
}

// Wheel events are quantised to WHEEL_DELTA notches, which is much coarser
// than a pad frame's movement — so the same remainder-carry the pointer path
// uses is what makes slow scrolling work at all here.
//
// Takes raw pad deltas (Y growing upward). Natural scrolling means the
// content follows the finger, which is the opposite sense to the wheel's own
// convention that positive is "away from the user" — hence the inversion
// here rather than at the call site.
void TrackpadInput::UpdateScroll(int dx, int dy) {
    if (m_scrollDir == ScrollDirection::Natural) { dx = -dx; dy = -dy; }

    // The remainder carries in wheel units, which is what lets the scale change
    // underneath it — a slider moved mid-swipe, or the system setting changing
    // — without the part already banked meaning something different.
    const float scale = ScrollScale();
    const float fy = static_cast<float>(dy) * scale + m_scrollRemY;
    const float fx = static_cast<float>(dx) * scale + m_scrollRemX;
    const LONG  iy = static_cast<LONG>(fy);
    const LONG  ix = static_cast<LONG>(fx);
    m_scrollRemY = fy - static_cast<float>(iy);
    m_scrollRemX = fx - static_cast<float>(ix);

    if (iy != 0) {
        INPUT input{};
        input.type         = INPUT_MOUSE;
        input.mi.dwFlags   = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>(iy);
        InputInjection::Send(input, "trackpad-scroll");
    }
    if (ix != 0) {
        INPUT input{};
        input.type         = INPUT_MOUSE;
        input.mi.dwFlags   = MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = static_cast<DWORD>(ix);
        InputInjection::Send(input, "trackpad-hscroll");
    }
}

// Circular scroll, used when the direction is Clockwise or CounterClockwise.
//
// The pad is the wheel: the thumb's angle around the centre is measured each
// frame, and every angular segment it crosses emits one wheel tick.
//
// The angle is measured clockwise from the right edge. The pad reports Y
// upward while the screen reports it downward, so negating Y is what makes
// "clockwise" the direction the thumb visibly travels. Clockwise scrolls the
// wheel down and CounterClockwise up.
void TrackpadInput::UpdateCircularScroll(int16_t x, int16_t y, bool newContact) {
    constexpr double kPi    = 3.14159265358979323846;
    constexpr double kTwoPi = 2.0 * kPi;

    if (newContact) {
        // The first frame only records where the gesture began. Emitting a
        // tick from it would make merely touching the pad scroll.
        m_haveScrollAngle   = false;
        m_scrollCircularRem = 0.0f;
    }

    const double angle = std::atan2(-static_cast<double>(y),
                                     static_cast<double>(x));
    if (!m_haveScrollAngle) {
        m_scrollAnglePrev = angle;
        m_haveScrollAngle = true;
        return;
    }

    // Shortest signed step from the previous frame. atan2 wraps by a full turn
    // at the ±pi seam; folding the difference keeps that seam from reading as
    // almost a whole revolution.
    double delta = angle - m_scrollAnglePrev;
    if (delta >  kPi) delta -= kTwoPi;
    if (delta < -kPi) delta += kTwoPi;
    m_scrollAnglePrev = angle;

    // One tick per segment crossed. The speed percentage scales the tick rate
    // the same way it scaled the swipe path, and the wheel correction keeps
    // the distance the same whatever Windows' lines-per-notch setting is.
    const double segmentAngle = kTwoPi / static_cast<double>(m_scrollSegments);
    double ticks = (delta / segmentAngle)
                 * (static_cast<double>(m_scrollSpeed) / 100.0);
    ticks *= static_cast<double>(
        InputInjection::WheelCorrection(InputInjection::WheelLinesPerNotch()));

    // Clockwise is wheel-down (negative); CounterClockwise is wheel-up.
    if (m_scrollDir == ScrollDirection::Clockwise) ticks = -ticks;

    m_scrollCircularRem += static_cast<float>(ticks);
    const LONG whole = static_cast<LONG>(m_scrollCircularRem);
    if (whole == 0) return;
    m_scrollCircularRem -= static_cast<float>(whole);

    INPUT input{};
    input.type         = INPUT_MOUSE;
    input.mi.dwFlags   = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(whole * WHEEL_DELTA);
    InputInjection::Send(input, "trackpad-scroll");
}

// ---------------------------------------------------------------------------
// Directional pad
// ---------------------------------------------------------------------------

// Shortest angular distance between two bearings, in degrees, always 0..180.
static double AngleDelta(double a, double b) {
    const double d = std::fmod(std::fabs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

// Which directions a sector presses. Eight-way diagonals press two, which is
// what a real d-pad does and what makes diagonal movement in a game possible;
// four-way rounds to the nearer single direction.
static uint8_t SectorDirs(int sector, DiagonalMode diagonals) {
    if (sector < 0) return DirNone;
    if (diagonals == DiagonalMode::FourWay) {
        switch (sector) {
        case 0:  return DirRight;
        case 1:  return DirUp;
        case 2:  return DirLeft;
        default: return DirDown;
        }
    }
    switch (sector) {
    case 0:  return DirRight;
    case 1:  return DirUp   | DirRight;
    case 2:  return DirUp;
    case 3:  return DirUp   | DirLeft;
    case 4:  return DirLeft;
    case 5:  return DirDown | DirLeft;
    case 6:  return DirDown;
    default: return DirDown | DirRight;
    }
}

int TrackpadInput::ResolveSector(double angleDeg) const {
    const bool   four  = m_diagonals == DiagonalMode::FourWay;
    const int    count = four ? 4 : 8;
    const double step  = four ? 90.0 : 45.0;
    const double half  = step / 2.0;

    // Hold the sector already being pressed until the thumb has cleared its
    // boundary by the hysteresis margin, so a thumb resting on a boundary
    // stays where it is instead of alternating.
    if (m_sector >= 0
            && AngleDelta(angleDeg, static_cast<double>(m_sector) * step)
                   <= half + SECTOR_HYSTERESIS_DEG)
        return m_sector;

    int s = static_cast<int>(std::floor((angleDeg + half) / step)) % count;
    if (s < 0) s += count;
    return s;
}

// Directions come from the click rather than from touch, so that resting a
// thumb on the pad presses nothing, and so the pad's three signals stay
// disjoint: touch is not zoned, a click in the middle is the click binding,
// and a click out in the ring is a direction.
void TrackpadInput::UpdateDirections(bool clicked, int16_t x, int16_t y) {
    if (!clicked) {
        m_clickHeld = false;
        m_sector    = -1;
        m_dirs      = DirNone;
        return;
    }

    const double r = std::hypot(static_cast<double>(x), static_cast<double>(y));

    if (!m_clickHeld) {
        // Rising edge. Decide the zone once, here, from the position at the
        // click itself — measurement says the centroid moves ~120 units in the
        // frame before a click and ~350 across three, against a boundary of
        // 12000 and a gap of nearly 12000 between deliberate centre and
        // deliberate direction presses. Looking back for a cleaner position
        // would be machinery for a correction two orders of magnitude smaller
        // than the thing it corrects.
        m_clickHeld     = true;
        m_clickInCentre = r < kPadRingRadius;
        m_sector        = -1;
    }

    if (m_clickInCentre) {
        m_dirs = DirNone;   // this press is the click binding, not a direction
        return;
    }

    // A ring press keeps following the thumb around the ring, which is what
    // lets a roll from up to right register as both in turn and what gives the
    // direction-change haptic something to fire on. It stops re-resolving once
    // the thumb slides inside the boundary: the zone is latched, so this press
    // can never become a centre click, and the angle gets noisy near the
    // middle where a small wobble spans whole sectors.
    if (r >= kPadRingRadius) {
        double angle = std::atan2(static_cast<double>(y), static_cast<double>(x))
                     * 180.0 / 3.14159265358979323846;
        if (angle < 0.0) angle += 360.0;
        m_sector = ResolveSector(angle);
    }
    m_dirs = SectorDirs(m_sector, m_diagonals);
}

// Reports a single frame that moved further than a finger plausibly can, which
// is what "I barely touch the pad and it scrolls a whole page" would look like
// from in here.
//
// Movement is integrated from position deltas, so dropped reports are harmless
// on their own: the displacement still adds up to the same total whether it
// arrives in ten frames or one. The case that is NOT harmless is a lift and a
// re-placement that we never saw the touch bit go false across — then the gap
// between two unrelated points is applied as one enormous swipe.
//
// The two are told apart by the time attached to each line rather than by the
// distance. Milliseconds since the previous touched frame at the report rate
// means the sensor really did jump that far in one frame, and the touch bit is
// lying about a finger that left. A long gap means reports went missing during
// a real movement, which is the benign kind and the reason there is no clamp
// here yet: a threshold picked without knowing which of these is happening
// would clip fast flicks on a transport that simply reports less often.
//
// The haptic tick path already distrusts jumps like this — see
// TRACKPAD_TICK_MAX_STEP in ControllerManager — but movement never learned to.
void TrackpadInput::NoteJump(const uint8_t* buf, size_t n, int dx, int dy) {
    const double dist = std::hypot(static_cast<double>(dx), static_cast<double>(dy));
    if (dist < JUMP_REPORT_UNITS) {
        m_lastFrameAt = std::chrono::steady_clock::now();
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const double sinceFrameMs = m_haveFrameTime
        ? std::chrono::duration<double, std::milli>(now - m_lastFrameAt).count()
        : -1.0;
    m_lastFrameAt   = now;
    m_haveFrameTime = true;

    // One line every few seconds. A pad that jumps once jumps often, and the
    // interesting part is the shape of a handful of them, not a flood.
    if (m_haveLoggedJump
            && std::chrono::duration<double>(now - m_lastJumpLogAt).count() < JUMP_LOG_GAP_S)
        return;
    m_lastJumpLogAt  = now;
    m_haveLoggedJump = true;

    uint16_t area = 0;
    if (n >= 30) std::memcpy(&area, buf + (m_isLeftPad ? 22 : 28), 2);

    EventLog::Write("TRACKPAD: %s pad moved %.0f units in one frame "
                    "(dx=%d dy=%d, %.1fms since the last touched frame, area=%u, mode=%s)",
                    m_isLeftPad ? "left" : "right", dist, dx, dy,
                    sinceFrameMs, area, TrackpadModeId(m_mode));
}

void TrackpadInput::Update(const uint8_t* buf, size_t n, bool pressed) {
    // Only three modes read the pad's position at all. A pad set to None or
    // feeding the DS4 touchpad has nothing for this class to work out, and a
    // single button is the click bit alone, which ControllerManager reads for
    // itself alongside every other binding.
    const bool wantsMovement = m_mode == TrackpadMode::MousePointer
                            || m_mode == TrackpadMode::ScrollWheel;
    const bool wantsDirections = m_mode == TrackpadMode::DirectionalPad;
    if (n < 30 || (!wantsMovement && !wantsDirections)) return;

    const uint8_t b2 = buf[4];
    const uint8_t b3 = buf[5];

    const bool touching = m_isLeftPad
        ? (b3 & SteamController::BTN_TP_LT) != 0
        : (b2 & SteamController::BTN_TP_RT) != 0;

    int16_t x = 0, y = 0;
    if (m_isLeftPad) {
        memcpy(&x, buf + 18, 2);
        memcpy(&y, buf + 20, 2);
    } else {
        memcpy(&x, buf + 24, 2);
        memcpy(&y, buf + 26, 2);
    }

    if (wantsDirections) {
        UpdateDirections(pressed, x, y);
        return;
    }

    // The controller streams reports whether or not a pad is touched, so the
    // same frames that carry finger movement also drive the glide. Clamping dt
    // keeps a stalled read from teleporting the cursor when reports resume.
    const auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (m_haveUpdateTime) {
        dt = std::chrono::duration<float>(now - m_lastUpdateAt).count();
        if (dt < 0.0f) dt = 0.0f;
        if (dt > GLIDE_MAX_DT_S) dt = GLIDE_MAX_DT_S;
    }
    m_lastUpdateAt   = now;
    m_haveUpdateTime = true;

    // A profile change can switch glide off while one is running. Stop it here,
    // on the thread that owns the motion, rather than from the UI thread.
    if (!m_glideEnabled) StopGlide();

    if (touching) {
        if (!m_touching) {
            // A new contact takes the cursor back immediately. Anything still
            // coasting belongs to the previous touch and must not fight the
            // thumb.
            StopGlide();
            m_velX           = 0.0f;
            m_velY           = 0.0f;
            m_haveVelocity   = false;
            m_contactPressed = false;
        }

        int dxRaw = 0, dyRaw = 0;
        if (m_touching) {
            dxRaw = static_cast<int>(x - m_prevX);
            dyRaw = static_cast<int>(y - m_prevY);
            NoteJump(buf, n, dxRaw, dyRaw);
        }

        if (m_mode == TrackpadMode::MousePointer) {
            if (dxRaw != 0 || dyRaw != 0) {
                // Pad Y grows upward, screen Y grows downward.
                UpdatePointer(dxRaw, -dyRaw);
            }
        } else if (IsCircularScroll(m_scrollDir)) {
            // Circular scrolling measures the absolute angle rather than the
            // frame's delta, so it runs even on a still frame: the first one
            // records where the gesture began.
            UpdateCircularScroll(x, y, !m_touching);
        } else if (dxRaw != 0 || dyRaw != 0) {
            // Raw deltas — UpdateScroll owns the direction convention.
            UpdateScroll(dxRaw, dyRaw);
        }

        if (m_mode == TrackpadMode::MousePointer) {
            if (pressed) m_contactPressed = true;
            // Fed even on a still frame: that is what lets a thumb that stops
            // before lifting decline to glide.
            const float sensitivity = PointerSensitivity();
            UpdateVelocity(dt,
                           static_cast<float>(dxRaw) * sensitivity,
                           static_cast<float>(-dyRaw) * sensitivity);
        }

        m_prevX = x;
        m_prevY = y;
    } else {
        if (m_touching) {
            // The lift. A click is a stop, not a flick: a glide out of a
            // button press would move the cursor the user was aiming with.
            if (m_mode == TrackpadMode::MousePointer && m_glideEnabled
                    && !m_contactPressed)
                StartGlide();
            m_velX           = 0.0f;
            m_velY           = 0.0f;
            m_haveVelocity   = false;
            m_contactPressed = false;
        }

        if (m_mode == TrackpadMode::MousePointer && m_glideActive)
            UpdateGlide(dt);
    }

    m_touching = touching;
}
