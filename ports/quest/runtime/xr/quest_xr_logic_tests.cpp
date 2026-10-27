// quest_xr_logic_tests: the stick-click chords, the in-world movement stick, the press latch, the screen
// placement and the in-world HUD's lazy follow of the Quest immersive mode (quest_xr_input_logic.h). Title-free; runs on the headset from
// quest-tools-arm64.zip (run_all.sh) or on any host.
#include "quest_xr_input_logic.h"

#include <cmath>
#include <cstdio>
#include <string_view>

namespace {

int failures = 0;

void Check(bool condition, const char* what, int line) {
    if (!condition) {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

bool Near(float a, float b, float epsilon = 1e-4F) { return std::fabs(a - b) <= epsilon; }

using namespace TriAevumQuestXr::Logic;

// Feeds a held state for `seconds` at 72 Hz and ORs every output.
ChordOutput Hold(StickChords& chords, double& now, double seconds, bool l3, bool r3, bool menu) {
    ChordOutput any;
    const double end = now + seconds;
    while (now < end) {
        const auto out = chords.Update(now, l3, r3, menu);
        any.SelectTap |= out.SelectTap;
        any.MenuTap |= out.MenuTap;
        any.Recenter |= out.Recenter;
        any.ToggleSettings |= out.ToggleSettings;
        any.StartHeld |= out.StartHeld;
        now += 1.0 / 72.0;
    }
    return any;
}

void TapsAndChords() {
    {  // L3 tap -> Select; R3 tap -> menu
        StickChords c;
        double t = 0;
        Hold(c, t, 0.1, false, false, false);
        auto down = Hold(c, t, 0.2, true, false, false);
        CHECK(!down.SelectTap && !down.Recenter);
        auto up = Hold(c, t, 0.05, false, false, false);
        CHECK(up.SelectTap && !up.MenuTap);
        Hold(c, t, 0.2, false, true, false);
        up = Hold(c, t, 0.05, false, false, false);
        CHECK(up.MenuTap && !up.SelectTap);
    }
    {  // a long press is no tap
        StickChords c;
        double t = 0;
        Hold(c, t, 0.6, true, false, false);
        CHECK(!Hold(c, t, 0.05, false, false, false).SelectTap);
    }
    {  // both held 1 s -> one recenter, no taps
        StickChords c;
        double t = 0;
        auto early = Hold(c, t, 0.9, true, true, false);
        CHECK(!early.Recenter);
        auto later = Hold(c, t, 1.0, true, true, false);
        CHECK(later.Recenter);
        auto release = Hold(c, t, 0.1, false, false, false);
        CHECK(!release.SelectTap && !release.MenuTap && !release.Recenter);
        // a second hold recenters again
        CHECK(Hold(c, t, 1.1, true, true, false).Recenter);
    }
    {  // both clicked briefly: neither tap fires
        StickChords c;
        double t = 0;
        Hold(c, t, 0.1, true, true, false);
        auto release = Hold(c, t, 0.05, false, false, false);
        CHECK(!release.SelectTap && !release.MenuTap && !release.Recenter);
    }
    {  // L3 held + menu -> settings; no Start and no Select, also after L3 is released first
        StickChords c;
        double t = 0;
        Hold(c, t, 0.1, true, false, false);
        auto chord = Hold(c, t, 0.1, true, false, true);
        CHECK(chord.ToggleSettings && !chord.StartHeld);
        auto l3Released = Hold(c, t, 0.1, false, false, true);
        CHECK(!l3Released.SelectTap && !l3Released.StartHeld && !l3Released.ToggleSettings);
        Hold(c, t, 0.1, false, false, false);
        // a plain menu press afterwards is Start again
        CHECK(Hold(c, t, 0.1, false, false, true).StartHeld);
    }
    {  // menu alone is Start
        StickChords c;
        double t = 0;
        auto start = Hold(c, t, 0.2, false, false, true);
        CHECK(start.StartHeld && !start.ToggleSettings);
    }
}

// Like Hold, in the in-world deliberate-menu mode with the right stick idle or not.
ChordOutput HoldWorld(StickChords& chords, double& now, double seconds, bool l3, bool r3, bool menu,
                      bool rightIdle = true) {
    ChordOutput any;
    const double end = now + seconds;
    while (now < end) {
        const auto out = chords.Update(now, l3, r3, menu, true, rightIdle);
        any.SelectTap |= out.SelectTap;
        any.MenuTap |= out.MenuTap;
        any.Recenter |= out.Recenter;
        any.ToggleSettings |= out.ToggleSettings;
        any.StartHeld |= out.StartHeld;
        now += 1.0 / 72.0;
    }
    return any;
}

void DeliberateMenu() {
    {  // in-world: an R3 tap (a stray click) never opens the menu
        StickChords c;
        double t = 0;
        HoldWorld(c, t, 0.2, false, true, false);
        CHECK(!HoldWorld(c, t, 0.1, false, false, false).MenuTap);
    }
    {  // in-world: R3 held 0.7 s with the stick centred opens it once, while held; the release is no tap
        StickChords c;
        double t = 0;
        CHECK(!HoldWorld(c, t, 0.6, false, true, false).MenuTap);
        int fired = 0;
        for (int frame = 0; frame < 72; ++frame) {
            fired += c.Update(t, false, true, false, true, true).MenuTap ? 1 : 0;
            t += 1.0 / 72.0;
        }
        CHECK(fired == 1);
        CHECK(!HoldWorld(c, t, 0.1, false, false, false).MenuTap);
        // the menu then shows the screen (Q3 mapping): the same release is still no tap
        StickChords d;
        double u = 0;
        CHECK(HoldWorld(d, u, 0.8, false, true, false).MenuTap);
        CHECK(!Hold(d, u, 0.1, false, false, false).MenuTap);
    }
    {  // in-world: R3 held while steering (stick deflected at any point) never opens it
        StickChords c;
        double t = 0;
        HoldWorld(c, t, 0.2, false, true, false, false);
        CHECK(!HoldWorld(c, t, 1.0, false, true, false, true).MenuTap);
        CHECK(!HoldWorld(c, t, 0.1, false, false, false).MenuTap);
    }
    {  // in-world: L3 joining the hold spoils it; both held still recenter
        StickChords c;
        double t = 0;
        HoldWorld(c, t, 0.3, false, true, false);
        const auto both = HoldWorld(c, t, 1.1, true, true, false);
        CHECK(both.Recenter && !both.MenuTap);
        CHECK(!HoldWorld(c, t, 0.1, false, false, false).MenuTap);
    }
    {  // the Q3 screen mapping is unchanged: the stick idle flag is ignored outside the world
        StickChords c;
        double t = 0;
        for (int frame = 0; frame < 14; ++frame) {
            c.Update(t, false, true, false, false, false);
            t += 1.0 / 72.0;
        }
        CHECK(c.Update(t, false, false, false, false, false).MenuTap);
    }
}

void MovementStick() {
    {  // the left stick alone
        const auto m = PickMoveStick(0.5F, 0.7F, true, 0.0F, 0.0F);
        CHECK(m.Source == StickSource::Left && Near(m.X, 0.5F) && Near(m.Y, 0.7F));
    }
    {  // left idle: the right stick moves Link
        const auto m = PickMoveStick(0.05F, 0.0F, true, -0.8F, 0.3F);
        CHECK(m.Source == StickSource::Right && Near(m.X, -0.8F) && Near(m.Y, 0.3F));
    }
    {  // left controller not active (Jamal's session): the right stick, whatever the left reads
        const auto m = PickMoveStick(0.9F, 0.0F, false, 0.0F, 0.4F);
        CHECK(m.Source == StickSource::Right && Near(m.X, 0.0F) && Near(m.Y, 0.4F));
        const auto idle = PickMoveStick(0.9F, 0.0F, false, 0.0F, 0.0F);
        CHECK(idle.Source == StickSource::None && Near(idle.X, 0.0F) && Near(idle.Y, 0.0F));
    }
    {  // both pushed: the larger deflection wins; a tie keeps the left
        CHECK(PickMoveStick(0.3F, 0.0F, true, 0.0F, -0.9F).Source == StickSource::Right);
        CHECK(PickMoveStick(0.0F, 1.0F, true, 0.6F, 0.0F).Source == StickSource::Left);
        CHECK(PickMoveStick(0.5F, 0.0F, true, 0.0F, 0.5F).Source == StickSource::Left);
    }
    {  // both idle: none, the left values pass through (drift stays below the game's dead zone)
        const auto m = PickMoveStick(0.1F, 0.0F, true, 0.1F, 0.1F);
        CHECK(m.Source == StickSource::None && Near(m.X, 0.1F));
    }
    {  // non-finite input is ignored
        CHECK(PickMoveStick(NAN, 0.0F, true, 0.0F, 0.6F).Source == StickSource::Right);
        CHECK(PickMoveStick(0.6F, 0.0F, true, INFINITY, 0.0F).Source == StickSource::Left);
    }
    CHECK(StickIdle(0.1F, 0.1F) && !StickIdle(0.2F, 0.0F) && StickIdle(NAN, 1.0F));
    {  // the log tracker: changes only, idle is no change
        StickSourceTracker tracker;
        CHECK(!tracker.Update(StickSource::None));
        CHECK(tracker.Update(StickSource::Right));
        CHECK(!tracker.Update(StickSource::Right));
        CHECK(!tracker.Update(StickSource::None));
        CHECK(tracker.Update(StickSource::Left));
        CHECK(std::string_view(StickSourceName(tracker.Last)) == "left");
    }
}

void LatchAndPulse() {
    PressLatch latch;
    latch.Observe(0x1);
    latch.Observe(0x0);  // released before the game polled
    CHECK(latch.Take(0x0) == 0x1);
    CHECK(latch.Take(0x0) == 0x0);
    latch.Observe(0x2);
    CHECK(latch.Take(0x2) == 0x2);
    latch.Observe(0x2);  // still held: no new press
    CHECK(latch.Take(0x0) == 0x0);

    Pulse pulse;
    CHECK(!pulse.Held(1.0));
    pulse.Fire(1.0);
    CHECK(pulse.Held(1.05) && pulse.Held(1.11) && !pulse.Held(1.13));

    Hysteresis grip;
    CHECK(!grip.Update(0.5F));
    CHECK(grip.Update(0.6F));
    CHECK(grip.Update(0.5F));
    CHECK(!grip.Update(0.4F));

    CHECK(AxisToSdl(1.0F) == 32767 && AxisToSdl(-1.0F) == -32767 && AxisToSdl(0.0F) == 0);
    CHECK(AxisToSdl(2.0F) == 32767 && AxisToSdl(NAN) == 0);
    CHECK(TriggerToSdl(0.5F) == 16384 && TriggerToSdl(-1.0F) == 0);
}

Quat YawPitch(float yaw, float pitch) {
    // q = qYaw(Y) * qPitch(X)
    const Quat y{0.0F, std::sin(yaw / 2), 0.0F, std::cos(yaw / 2)};
    const Quat p{std::sin(pitch / 2), 0.0F, 0.0F, std::cos(pitch / 2)};
    return {y.W * p.X + y.X * p.W + y.Y * p.Z - y.Z * p.Y, y.W * p.Y - y.X * p.Z + y.Y * p.W + y.Z * p.X,
            y.W * p.Z + y.X * p.Y - y.Y * p.X + y.Z * p.W, y.W * p.W - y.X * p.X - y.Y * p.Y - y.Z * p.Z};
}

void Placement() {
    const float pi = 3.14159265F;
    {
        const auto s = PlaceScreen(Quat{}, Vec3{0.1F, 1.6F, 0.2F}, 2.5F);
        CHECK(Near(s.YawRadians, 0.0F));
        CHECK(Near(s.Position.X, 0.1F) && Near(s.Position.Y, 1.6F) && Near(s.Position.Z, -2.3F));
        CHECK(Near(s.Orientation.Y, 0.0F) && Near(s.Orientation.W, 1.0F));
    }
    {  // turned 90 degrees left: the screen is on -X, facing +X
        const auto s = PlaceScreen(YawPitch(pi / 2, 0.0F), Vec3{}, 2.0F);
        CHECK(Near(s.YawRadians, pi / 2, 1e-3F));
        CHECK(Near(s.Position.X, -2.0F, 1e-3F) && Near(s.Position.Z, 0.0F, 1e-3F));
    }
    {  // pitch does not move the screen up or down, nor change its yaw
        const auto s = PlaceScreen(YawPitch(pi / 4, -0.5F), Vec3{0.0F, 1.2F, 0.0F}, 2.5F);
        CHECK(Near(s.YawRadians, pi / 4, 1e-3F));
        CHECK(Near(s.Position.Y, 1.2F));
    }
    {  // looking straight down still yields the body's yaw
        const auto s = PlaceScreen(YawPitch(-pi / 3, -pi / 2), Vec3{}, 2.5F);
        CHECK(Near(s.YawRadians, -pi / 3, 1e-2F));
    }
    CHECK(Near(ScreenHeight(3.0F, 1280, 720), 1.6875F));
}

// v' = q * v * q^-1
Vec3 RotateBy(const Quat& q, const Vec3& v) {
    const float tx = 2.0F * (q.Y * v.Z - q.Z * v.Y), ty = 2.0F * (q.Z * v.X - q.X * v.Z),
                tz = 2.0F * (q.X * v.Y - q.Y * v.X);
    return {v.X + q.W * tx + (q.Y * tz - q.Z * ty), v.Y + q.W * ty + (q.Z * tx - q.X * tz),
            v.Z + q.W * tz + (q.X * ty - q.Y * tx)};
}

float Deg(float degrees) { return degrees * 3.14159265F / 180.0F; }

void HudFollowTests() {
    const HudFollowConfig config;
    const float dt = 1.0F / 90.0F;
    {  // The first frame snaps in front of the head: 2.5 m out along the yaw, 8 deg below eye level, facing it.
        HudFollow hud;
        const Vec3 head{0.1F, 1.6F, 0.2F};
        const auto p = hud.Update(YawPitch(Deg(30.0F), Deg(-20.0F)), head, dt, config);
        CHECK(hud.Valid && hud.Placements == 1U && !hud.Following);
        CHECK(Near(p.YawRadians, Deg(30.0F), 1e-4F));
        const Vec3 offset{p.Position.X - head.X, p.Position.Y - head.Y, p.Position.Z - head.Z};
        CHECK(Near(std::sqrt(offset.X * offset.X + offset.Y * offset.Y + offset.Z * offset.Z), 2.5F, 1e-4F));
        CHECK(Near(offset.Y, -2.5F * std::sin(Deg(8.0F)), 1e-4F));  // head pitch does not move it
        // Its -Z (into the quad) points from the head to its centre: the front face looks at the head.
        const Vec3 forward = RotateBy(p.Orientation, {0.0F, 0.0F, -1.0F});
        CHECK(Near(forward.X, offset.X / 2.5F, 1e-4F) && Near(forward.Y, offset.Y / 2.5F, 1e-4F) &&
              Near(forward.Z, offset.Z / 2.5F, 1e-4F));
        const Vec3 up = RotateBy(p.Orientation, {0.0F, 1.0F, 0.0F});
        CHECK(up.Y > 0.99F);  // no roll, a small tilt
    }
    {  // Turns inside the 35 deg dead zone leave the yaw alone; the position still follows the head.
        HudFollow hud;
        hud.Update(YawPitch(0.0F, 0.0F), Vec3{}, dt, config);
        for (int frame = 0; frame < 90; ++frame) {
            const float yaw = Deg(30.0F) * std::sin(frame * 0.2F);
            const auto p = hud.Update(YawPitch(yaw, 0.0F), Vec3{0.5F, 1.7F, -0.3F}, dt, config);
            CHECK(Near(p.YawRadians, 0.0F) && !hud.Following);
            CHECK(Near(p.Position.X, 0.5F, 1e-4F) && Near(p.Position.Z, -0.3F - 2.5F * std::cos(Deg(8.0F)), 1e-4F));
        }
        CHECK(hud.Placements == 1U);
    }
    {  // A 50 deg turn: the catch-up starts, approaches without overshoot, and settles within 2 deg in ~1 s.
        HudFollow hud;
        hud.Update(YawPitch(0.0F, 0.0F), Vec3{}, dt, config);
        float previous = 0.0F;
        int settledFrame = -1;
        for (int frame = 0; frame < 180; ++frame) {
            const auto p = hud.Update(YawPitch(Deg(50.0F), 0.0F), Vec3{}, dt, config);
            CHECK(p.YawRadians >= previous - 1e-6F && p.YawRadians <= Deg(50.0F) + 1e-5F);
            if (frame == 0) CHECK(hud.Placements == 2U && p.YawRadians > 0.0F && p.YawRadians < Deg(5.0F));
            previous = p.YawRadians;
            if (!hud.Following && settledFrame < 0) settledFrame = frame;
        }
        CHECK(settledFrame > 45 && settledFrame < 120);
        CHECK(std::fabs(previous - Deg(50.0F)) <= Deg(2.0F));
        CHECK(hud.Placements == 2U);
    }
    {  // Across the +-180 deg seam: 20 deg apart is inside the dead zone; 40 deg catches up the short way.
        HudFollow hud;
        hud.Update(YawPitch(Deg(170.0F), 0.0F), Vec3{}, dt, config);
        hud.Update(YawPitch(Deg(-170.0F), 0.0F), Vec3{}, dt, config);
        CHECK(!hud.Following && Near(hud.Yaw, Deg(170.0F), 1e-4F));
        for (int frame = 0; frame < 200; ++frame) {
            hud.Update(YawPitch(Deg(-150.0F), 0.0F), Vec3{}, dt, config);
            CHECK(std::fabs(hud.Yaw) >= Deg(149.0F));  // never through 0
        }
        CHECK(std::fabs(WrapRadians(hud.Yaw - Deg(-150.0F))) <= Deg(2.0F) && !hud.Following);
    }
    {  // Reset snaps the next frame to the head; a bad dt freezes a catch-up instead of jumping.
        HudFollow hud;
        hud.Update(YawPitch(0.0F, 0.0F), Vec3{}, dt, config);
        hud.Update(YawPitch(Deg(90.0F), 0.0F), Vec3{}, NAN, config);
        CHECK(hud.Following && Near(hud.Yaw, 0.0F));
        hud.Update(YawPitch(Deg(90.0F), 0.0F), Vec3{}, -1.0F, config);
        CHECK(Near(hud.Yaw, 0.0F));
        hud.Reset();
        const auto p = hud.Update(YawPitch(Deg(-120.0F), 0.0F), Vec3{}, dt, config);
        CHECK(Near(p.YawRadians, Deg(-120.0F), 1e-4F) && !hud.Following && hud.Placements == 3U);
    }
    {  // Sizing defaults: nearer than Link, ~39 deg wide, and the edge stays within 40 deg of the gaze.
        CHECK(Near(DefaultHudDistanceM(1.8F), 1.5F));            // the default placement
        CHECK(Near(DefaultHudDistanceM(1.0F), 1.0F) && Near(DefaultHudDistanceM(0.8F), 1.0F));
        CHECK(Near(DefaultHudDistanceM(4.0F), 2.5F));
        CHECK(Near(DefaultHudDistanceM(NAN), 1.5F) && Near(DefaultHudDistanceM(-1.0F), 1.5F));
        for (float place = 0.8F; place <= 4.0F; place += 0.1F) {
            const float distance = DefaultHudDistanceM(place);
            if (place >= 1.3F) CHECK(distance < place);  // in front of Link wherever the clamp allows
            const float width = DefaultHudWidthM(distance);
            const float half = HudHalfAngleRadians(width, distance);
            CHECK(half > Deg(18.0F) && half < Deg(21.0F));  // constant angular size
            const float deadzone = DefaultHudDeadzoneRadians(width, distance);
            CHECK(Near(deadzone + half, Deg(40.0F), 1e-4F));
        }
        // W4-minimal's old size (2.2 m at 2.5 m, 23.7 deg half) gets a 16.3 deg dead zone, not 35.
        CHECK(Near(DefaultHudDeadzoneRadians(2.2F, 2.5F), Deg(40.0F) - std::atan(0.44F), 1e-4F));
        CHECK(Near(DefaultHudDeadzoneRadians(8.0F, 1.0F), Deg(5.0F)));  // a huge quad: the floor
        CHECK(HudHalfAngleRadians(1.0F, 0.0F) == 0.0F && HudHalfAngleRadians(NAN, 1.0F) == 0.0F);

        // With the defaults, turning just inside the dead zone keeps the far edge within 40 deg of
        // the gaze, and any steady turn beyond it catches up until the edge is back near the centre.
        HudFollowConfig defaults;
        defaults.DistanceM = DefaultHudDistanceM(1.8F);
        const float width = DefaultHudWidthM(defaults.DistanceM);
        const float half = HudHalfAngleRadians(width, defaults.DistanceM);
        defaults.DeadzoneRadians = DefaultHudDeadzoneRadians(width, defaults.DistanceM);
        HudFollow hud;
        hud.Update(YawPitch(0.0F, 0.0F), Vec3{}, dt, defaults);
        hud.Update(YawPitch(defaults.DeadzoneRadians - Deg(0.5F), 0.0F), Vec3{}, dt, defaults);
        CHECK(!hud.Following && std::fabs(hud.Yaw - (defaults.DeadzoneRadians - Deg(0.5F))) + half <= Deg(40.0F));
        for (int frame = 0; frame < 180; ++frame) hud.Update(YawPitch(Deg(60.0F), 0.0F), Vec3{}, dt, defaults);
        CHECK(!hud.Following && std::fabs(WrapRadians(Deg(60.0F) - hud.Yaw)) <= Deg(2.0F));
    }
    CHECK(Near(WrapRadians(Deg(190.0F)), Deg(-170.0F), 1e-4F) && Near(WrapRadians(Deg(-190.0F)), Deg(170.0F), 1e-4F));
    CHECK(WrapRadians(NAN) == 0.0F);
}

}  // namespace

void DisplayTicks() {
    constexpr int64_t kPeriod = 11'111'111;  // 90 Hz
    DisplayTickClock clock;
    int64_t t = 5'000'000'000;
    CHECK(clock.Advance(t, kPeriod) == 1U);
    CHECK(clock.Advance(t += kPeriod, kPeriod) == 2U);
    CHECK(clock.Advance(t += kPeriod + 900'000, kPeriod) == 3U);  // jitter is one period
    CHECK(clock.Advance(t += 3 * kPeriod, kPeriod) == 6U);        // a late wait: two periods skipped
    CHECK(clock.Skipped == 2U);
    CHECK(clock.Advance(t, kPeriod) == 7U);                       // the same time again: still monotonic
    // A refresh switch (90 -> 72 Hz): one period of the new rate is one tick.
    constexpr int64_t k72 = 13'888'889;
    CHECK(clock.Advance(t += k72, k72) == 8U);
    // A stopped session's gap is not counted; the tick goes on by one.
    clock.Pause();
    const uint64_t skipped = clock.Skipped;
    CHECK(clock.Advance(t += 50 * k72, k72) == 9U && clock.Skipped == skipped);
    CHECK(clock.Advance(t += k72, k72) == 10U);
}

int main() {
    TapsAndChords();
    DeliberateMenu();
    MovementStick();
    LatchAndPulse();
    Placement();
    HudFollowTests();
    DisplayTicks();
    if (failures != 0) {
        std::printf("quest_xr_logic_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("quest_xr_logic_tests: all passed\n");
    return 0;
}
