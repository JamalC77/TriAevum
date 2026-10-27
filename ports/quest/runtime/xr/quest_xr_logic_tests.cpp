// quest_xr_logic_tests: the stick-click chords, the press latch and the screen placement of the
// Quest immersive mode (quest_xr_input_logic.h). Title-free; runs on the headset from
// quest-tools-arm64.zip (run_all.sh) or on any host.
#include "quest_xr_input_logic.h"

#include <cmath>
#include <cstdio>

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
    LatchAndPulse();
    Placement();
    DisplayTicks();
    if (failures != 0) {
        std::printf("quest_xr_logic_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("quest_xr_logic_tests: all passed\n");
    return 0;
}
