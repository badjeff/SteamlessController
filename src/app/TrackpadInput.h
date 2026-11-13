#pragma once
#include <chrono>
#include <cstdint>
#include <cstddef>
#include "TrackpadConfig.h"

// What ONE physical trackpad's finger position means. One instance per pad, so
// the two pads can be configured independently — this used to be a single
// instance switched between pads by a bool, which structurally could not
// represent "both pads doing something".
//
// Two jobs, both of them "turn a position into something". The pointer and
// scroll modes drive the desktop directly from here. Directional pad mode
// resolves nothing itself: it works out which direction the thumb is pressing
// and hands that out through Directions(), because a direction has to reach
// both the virtual gamepad and SendInput and neither of those belongs here.
//
// No binding is dispatched from this class. A pad's click, touch and
// directions are all BackButtonBindings, dispatched by ControllerManager
// alongside the back paddles, which is what lets them be the same kind of
// thing as a paddle whatever this pad's mode is.
class TrackpadInput {
public:
    // Which physical pad to read. Set once when the slot is created.
    void SetPad(bool isLeftPad) { m_isLeftPad = isLeftPad; }
    void SetMode(TrackpadMode mode);
    void SetScrollDirection(ScrollDirection dir) {
        if (dir == m_scrollDir) return;
        m_scrollDir = dir;
        // The stored angle belongs to the old gesture, so it cannot be
        // compared with the next frame's.
        m_haveScrollAngle   = false;
        m_scrollCircularRem = 0.0f;
    }
    // Percent of the built-in scale, for both scroll and pointer movement — see
    // kScrollSpeedDefault. Clamped here rather than trusted, since it arrives
    // from the registry.
    void SetScrollSpeed(uint32_t percent) { m_scrollSpeed = ClampScrollSpeed(percent); }
    // Circular scroll only: angular segments per revolution. Clamped here for
    // the same reason as the speed.
    void SetScrollSegments(uint32_t segments) {
        m_scrollSegments = ClampScrollSegments(segments);
    }
    void SetDiagonals(DiagonalMode d);
    // Opt-in inertial glide, MousePointer only. Turning it off stops a glide
    // already in progress on the next report rather than leaving it running.
    void SetGlide(bool enabled) { m_glideEnabled = enabled; }
    // Release speed that starts a coast, px/s (0 = coast on every release that
    // moved), and the percentage of speed kept per GLIDE_DECAY_TICK_S.
    void SetGlideSpeed(uint32_t px) {
        m_glideSpeedPx = static_cast<float>(GlideSpeedFromDword(px));
    }
    void SetGlideDecay(uint32_t pct) {
        m_glideDecay = static_cast<float>(GlideDecayFromDword(pct)) / 100.0f;
    }

    // `pressed` is whether the pad is being pressed, worked out from contact
    // area by the caller — the firmware's click bit reports fewer than half of
    // them. See kPadPressArea.
    void Update(const uint8_t* buf, size_t n, bool pressed);
    void Reset();

    // Which directions this pad is pressing right now, as PadDir bits. Always
    // empty unless the pad is a directional pad with its click held.
    uint8_t Directions() const { return m_dirs; }

    // Where the click being held landed: inside the centre circle, where the
    // press is the pad's click binding, or out in the ring, where it is a
    // direction instead. True for any pad that is not a directional pad, so a
    // caller can gate the click binding on this unconditionally.
    //
    // Meaningless while no click is held, which costs nothing — every caller
    // is already testing the click bit.
    bool ClickInCentre() const { return m_clickInCentre; }

private:
    void UpdatePointer(int dx, int dy);
    // Pixel-space injection, shared by finger movement and the glide. Keeps the
    // sub-pixel remainder so a slow glide still moves the cursor instead of
    // rounding every frame to zero.
    void EmitPointer(float dxPx, float dyPx);
    float PointerSensitivity() const;
    float ScrollScale() const;
    void UpdateScroll(int dx, int dy);
    void UpdateCircularScroll(int16_t x, int16_t y, bool newContact);
    void UpdateVelocity(float dt, float dxPx, float dyPx);
    void StartGlide();
    void UpdateGlide(float dt);
    void StopGlide();
    void NoteMovementSent(long px, bool haveCursor, long cursorX, long cursorY);
    // Resolves the eight- or four-way sector a press at this angle belongs to,
    // holding the current sector until the thumb clears its boundary by the
    // hysteresis margin. Reads m_sector; does not write it.
    int  ResolveSector(double angleDeg) const;
    void UpdateDirections(bool clicked, int16_t x, int16_t y);
    // Diagnostic only — reports single frames that moved further than a finger
    // can, and how long since the previous one, which is what separates a
    // sensor jump from reports going missing. Changes nothing about what is
    // sent. See the definition.
    void NoteJump(const uint8_t* buf, size_t n, int dx, int dy);

    bool            m_isLeftPad = false;
    TrackpadMode    m_mode      = TrackpadMode::None;
    ScrollDirection m_scrollDir = ScrollDirection::Natural;
    uint32_t        m_scrollSpeed = kScrollSpeedDefault;
    DiagonalMode    m_diagonals = DiagonalMode::EightWay;
    bool            m_glideEnabled = false;

    bool     m_touching  = false;
    int16_t  m_prevX     = 0;
    int16_t  m_prevY     = 0;
    float    m_remX      = 0.0f;  // sub-pixel movement carry
    float    m_remY      = 0.0f;
    float    m_scrollRemX = 0.0f;  // sub-detent scroll carry
    float    m_scrollRemY = 0.0f;

    // ---- Circular scroll ----
    // Active instead of the swipe path for a Clockwise/CounterClockwise
    // direction. Keeps the previous angle and the sub-tick carry; the angle is
    // measured clockwise from the right edge (see UpdateCircularScroll).
    uint32_t m_scrollSegments    = kScrollSegmentsDefault;
    double   m_scrollAnglePrev   = 0.0;
    bool     m_haveScrollAngle   = false;
    float    m_scrollCircularRem = 0.0f;  // sub-tick carry, in wheel ticks

    // ---- Inertial glide ----
    // A flick keeps the cursor moving after the thumb leaves, slowing by a
    // configurable fraction each reference tick. It is driven from the pad's
    // own report frames rather than a timer, advancing by elapsed time so the
    // feel is the same at 60 Hz and 250 Hz. Velocities are in screen pixels
    // per second, so the speed slider is baked in when the glide starts.
    bool  m_glideActive = false;
    float m_glideVX     = 0.0f;
    float m_glideVY     = 0.0f;
    // Per-pad tuning from the profile: the release speed that starts a coast
    // (px/s, 0 = always) and the fraction of speed kept per reference tick.
    float m_glideSpeedPx = static_cast<float>(kGlideSpeedDefault);
    float m_glideDecay   = static_cast<float>(kGlideDecayDefault) / 100.0f;
    // Time spent coasting, for the duration cap. Reset at launch, not only on
    // stop, so the cap measures one glide rather than every glide so far.
    float m_glideElapsedS = 0.0f;

    // The finger's velocity, smoothed over roughly GLIDE_VELOCITY_TAU_S so one
    // dropped frame or a few units of jitter cannot become a wild flick.
    float m_velX = 0.0f;
    float m_velY = 0.0f;
    bool  m_haveVelocity = false;

    // Whether the contact that just ended was also a press. A click is a
    // deliberate stop, not a flick. Set for the whole contact, because the
    // press detector can report on any frame of it.
    bool m_contactPressed = false;

    // When the pad was last updated, for the frame's dt. Reset when the mode
    // changes or the slot is torn down so the first frame of a new mode cannot
    // inherit a long gap and fling.
    std::chrono::steady_clock::time_point m_lastUpdateAt{};
    bool m_haveUpdateTime = false;

    // Below the configured start speed a lift is a stop rather than a flick.
    // The threshold itself is per profile — see m_glideSpeedPx — because how
    // hard a flick has to be is taste. The tail is cut here rather than
    // allowed to crawl for seconds: 6 px/s is under one pixel per 60 Hz frame.
    static constexpr float GLIDE_STOP_SPEED_PX = 6.0f;
    // Ceiling for a sensor jump or an implausibly fast report becoming a
    // cursor across the screen. A deliberate flick is well under this.
    static constexpr float GLIDE_MAX_SPEED_PX = 5000.0f;
    // How quickly the finger's measured velocity follows the finger. Short
    // enough that the velocity at the lift is the flick's, long enough to
    // average out one noisy frame.
    static constexpr float GLIDE_VELOCITY_TAU_S = 0.030f;
    // The tick the configured decay percentage is quoted against. pct^(dt/this)
    // keeps the curve continuous whatever the report rate.
    static constexpr float GLIDE_DECAY_TICK_S = 0.016f;
    // A glide can never run forever, even at 100% decay.
    static constexpr float GLIDE_MAX_DURATION_S = 2.95f;
    // A report gap longer than this is a stall, not a frame. Advancing the
    // glide by the real gap would teleport the cursor after a hitch.
    static constexpr float GLIDE_MAX_DT_S = 0.050f;

    // Movement Windows accepted, checked against the cursor actually moving.
    // A refused SendInput reports itself; motion that is accepted and then
    // discarded — by a cursor clip a game left behind, or a low-level hook
    // filtering injected input — looks identical to the user and otherwise
    // leaves no trace at all. Records where the cursor was and how much travel
    // has been sent since; when the travel adds up and the position has not
    // changed, that is worth a log line.
    long     m_refCursorX  = 0;
    long     m_refCursorY  = 0;
    bool     m_haveRef     = false;
    long     m_travelSent  = 0;

    // Enough sent movement that a still cursor cannot be a coincidence.
    static constexpr long  STUCK_TRAVEL_PX = 120;

    static constexpr float SENSITIVITY = 0.01125f;
    // Chosen so a full swipe across the pad is a few notches rather than a
    // page-length fling: each pad axis saturates at +/-32767, and one wheel
    // detent is WHEEL_DELTA (120).
    static constexpr float SCROLL_SENSITIVITY = 0.02f;

    // ---- Directional pad ----

    // The directions currently pressed, and the sector they came from. The
    // sector is kept separately because hysteresis needs to know which one is
    // being held, which the bitmask cannot say: up-left and left-up are the
    // same two bits.
    uint8_t m_dirs   = DirNone;
    int     m_sector = -1;      // -1 = none held

    // A click is being held, and where it landed. The zone is decided once, on
    // the press, and never revisited: sliding from the ring into the centre
    // mid-press would otherwise release a direction and fire the click
    // binding, which is one physical press meaning two different things — the
    // exact collision the zone split exists to prevent.
    bool m_clickHeld     = false;
    bool m_clickInCentre = true;

    // How far past a sector boundary the thumb has to travel before the
    // direction changes. Without it a thumb parked on a boundary alternates
    // between two directions at the report rate, and since directions are
    // edge-dispatched that is a stream of key down/up pairs rather than a
    // cosmetic flicker.
    static constexpr double SECTOR_HYSTERESIS_DEG = 8.0;

    // ---- Movement jump reporting (diagnostic) ----

    // Far enough in one frame that no finger did it. The pad's axes saturate
    // at +/-32767, so this is roughly a fifth of the way across in a single
    // report — well past a fast flick at any report rate either transport
    // uses, and nowhere near the pad-width gap a missed lift produces.
    static constexpr double JUMP_REPORT_UNITS = 6000.0;
    // A pad that jumps once jumps often. A handful of examples says everything
    // a flood would, so the rest are dropped.
    static constexpr double JUMP_LOG_GAP_S = 3.0;

    std::chrono::steady_clock::time_point m_lastFrameAt{};
    std::chrono::steady_clock::time_point m_lastJumpLogAt{};
    bool m_haveFrameTime  = false;
    bool m_haveLoggedJump = false;
};
