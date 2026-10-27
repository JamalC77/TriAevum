// oot3d_xr_camera_logic_tests: the pure decisions of mode R (oot3d_xr_camera_logic.h; quest-research/
// inworld-spec.md §4.2-4.4, §6, §7, W2). Title-free and dependency-free; runs on any host (PC, or the
// headset through adb).
#include "oot3d_xr_camera_logic.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace {

using namespace Oot3dNativeGame::XrCamera;
namespace M = Oot3dNativeGame::XrMath;

int checks = 0;
int failures = 0;
int sectionChecks = 0;
int sectionFailures = 0;

void Check(bool condition, const char* what, int line) {
    ++checks;
    if (!condition) {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

void Report(const char* section) {
    std::printf("%-14s %4d checks, %d failures\n", section, checks - sectionChecks, failures - sectionFailures);
    sectionChecks = checks;
    sectionFailures = failures;
}

bool Near(double a, double b, double epsilon = 1e-6) { return std::fabs(a - b) <= epsilon; }
bool NearVec(const M::Vec3& a, const M::Vec3& b, double epsilon = 1e-6) {
    return Near(a.X, b.X, epsilon) && Near(a.Y, b.Y, epsilon) && Near(a.Z, b.Z, epsilon);
}
double Deg(double degrees) { return M::DegreesToRadians(degrees); }

// ---------------------------------------------------------------------------------------------------
void TestCameraWrite() {
    {
        const auto w = BuildCameraWrite({10, 50, 20}, {0, 0, 1}, {0, 0, 1});
        CHECK(w.Valid);
        CHECK(NearVec(w.Eye, {10, 50, 20}));
        CHECK(NearVec(w.At, {10, 50, 120}));
        CHECK(w.YawBin == 0 && w.PitchBin == 0);
    }
    {
        const auto w = BuildCameraWrite({0, 0, 0}, {1, 0, 0}, {1, 0, 0});
        CHECK(w.Valid && w.YawBin == 0x4000);
        CHECK(NearVec(w.At, {100, 0, 0}, 1e-9));
    }
    {
        // 30 deg up: the decomp's geo pitch (eye -> at rises), binary round(30 * 32768 / 180) = 5461.
        const M::Vec3 f{0, std::sin(Deg(30)), std::cos(Deg(30))};
        const auto w = BuildCameraWrite({0, 0, 0}, f, {0, 0, 1});
        CHECK(w.Valid && Near(w.Pitch, Deg(30), 1e-9) && w.PitchBin == 5461);
        CHECK(NearVec(w.At, {0, 50, 100 * std::cos(Deg(30))}, 1e-9));
    }
    {
        // Clamped to +-60 deg; roll never appears (level forward carries the yaw).
        const M::Vec3 f{0, std::sin(Deg(80)), std::cos(Deg(80))};
        const auto w = BuildCameraWrite({0, 0, 0}, f, {0, 0, 1});
        CHECK(Near(w.Pitch, Deg(60), 1e-9));
        const auto down = BuildCameraWrite({0, 0, 0}, {0, -1, 0}, {0, 0, 1});
        CHECK(down.Valid && Near(down.Pitch, Deg(-60), 1e-9) && down.YawBin == 0);
    }
    {
        // Yaw 180 deg -> 0x8000; the level forward wins over a pitched gaze.
        const auto w = BuildCameraWrite({0, 0, 0}, {0, 0.5, -0.8}, {0, 0, -1});
        CHECK(w.Valid && static_cast<uint16_t>(w.YawBin) == 0x8000U);
    }
    {
        // Invalid inputs.
        CHECK(!BuildCameraWrite({NAN, 0, 0}, {0, 0, 1}, {0, 0, 1}).Valid);
        CHECK(!BuildCameraWrite({0, 0, 0}, {0, 0, 1}, {0, 1, 0}).Valid);  // no level heading
        CHECK(!BuildCameraWrite(GameHead{}).Valid);
    }
    {
        // TopScreen's orbit convention (eye = at - d (cos p sin y, sin p, cos p cos y)): HoldCameraWrite of a
        // write recovers its angles.
        for (double yaw : {-170.0, -45.0, 0.0, 30.0, 135.0}) {
            for (double pitch : {-50.0, 0.0, 20.0}) {
                const M::Vec3 f{std::cos(Deg(pitch)) * std::sin(Deg(yaw)), std::sin(Deg(pitch)),
                                std::cos(Deg(pitch)) * std::cos(Deg(yaw))};
                const auto w = BuildCameraWrite({3, 4, 5}, f, M::NormalizedXZ(f, {0, 0, 1}));
                const auto h = HoldCameraWrite(w.Eye, w.At);
                CHECK(h.Valid && h.YawBin == w.YawBin && h.PitchBin == w.PitchBin);
            }
        }
        CHECK(!HoldCameraWrite({1, 1, 1}, {1, 1, 1}).Valid);
    }
    {
        // The head through the rig: yaw psi, an unrolled head facing -Z at 1.6 m looks along psi.
        M::Rig rig;
        rig.Anchor = {100, 10, -50};
        rig.Yaw = Deg(40);
        rig.Scale = 33.0;
        M::Pose head;
        head.Position = {0, 1.6, 0};
        auto g = HeadInGame(rig, head, false);
        CHECK(g.Valid && Near(g.Yaw, Deg(40), 1e-9) && Near(g.VrYaw, Deg(40), 1e-9));
        CHECK(NearVec(g.Position, {100, 10 + 1.6 * 33.0, -50}, 1e-9));
        g = HeadInGame(rig, head, true);  // mirrored: x and yaw negated, the VR yaw kept
        CHECK(Near(g.Yaw, Deg(-40), 1e-9) && Near(g.VrYaw, Deg(40), 1e-9) && Near(g.Position.X, -100, 1e-9));
        // Straight down: the level heading stays psi (the head's up vector carries it).
        head.Orientation = M::QuatFromAxisAngle({1, 0, 0}, Deg(-90));
        g = HeadInGame(rig, head, false);
        CHECK(g.Valid && Near(g.Yaw, Deg(40), 1e-6));
        const auto w = BuildCameraWrite(g);
        CHECK(w.Valid && Near(w.Pitch, Deg(-60), 1e-9));
    }
    {
        CHECK(Near(ParseCullFov(nullptr), 130.0) && Near(ParseCullFov("100"), 100.0));
        CHECK(Near(ParseCullFov("200"), 150.0) && Near(ParseCullFov("10"), 60.0) && Near(ParseCullFov("x"), 130.0));
    }
    {
        // The cull FOV past the pitch clamp: unchanged while the eye's far edge (55 deg) stays inside
        // limit + fov / 2, then just wide enough, capped at 150.
        for (double pitch : {0.0, 30.0, 60.0, 70.0, -70.0}) CHECK(Near(CullFovForPitch(130.0, Deg(pitch)), 130.0, 1e-9));
        CHECK(Near(CullFovForPitch(130.0, Deg(75)), 140.0, 1e-9) && Near(CullFovForPitch(130.0, Deg(-75)), 140.0, 1e-9));
        CHECK(Near(CullFovForPitch(130.0, Deg(-80)), 150.0, 1e-9) && Near(CullFovForPitch(130.0, Deg(-90)), 150.0, 1e-9));
        CHECK(Near(CullFovForPitch(100.0, Deg(-62)), 114.0, 1e-9));  // a narrower setting widens sooner
        CHECK(Near(CullFovForPitch(150.0, Deg(-90)), 150.0, 1e-9));
        for (double pitch = -90.0; pitch <= 90.0; pitch += 1.0) {
            // Covered wherever the cap allows: limit + fov / 2 >= |pitch| + edge.
            const double fov = CullFovForPitch(130.0, Deg(pitch));
            CHECK(fov >= 130.0 && fov <= 150.0);
            CHECK(fov >= 150.0 - 1e-9 || 60.0 + fov / 2.0 >= std::fabs(pitch) + kEyeEdgeDegrees - 1e-9);
        }
        CHECK(Near(CullFovForPitch(130.0, NAN), 130.0) && Near(CullFovForPitch(NAN, 0.0), 130.0));
        GameHead down;
        down.Valid = true;
        down.Forward = {0, -2, 0};
        CHECK(Near(HeadPitch(down), Deg(-90), 1e-12) && Near(HeadPitch(GameHead{}), 0.0));
    }
    Report("camera_write");
}

// ---------------------------------------------------------------------------------------------------
OwnerInput Gameplay() {
    OwnerInput in;
    in.LayerWorld = true;
    in.RigValid = true;
    in.HeadValid = true;
    in.PlayerValid = true;
    return in;
}

void TestOwner() {
    {
        const auto d = DecideOwner(Gameplay());
        CHECK(d.Vr && !d.Probe && d.Held == 0U && !d.RigFrozen && !d.StickBypass);
    }
    {
        auto in = Gameplay();
        in.MainCamera = false;
        const auto d = DecideOwner(in);
        CHECK(!d.Vr && d.Held == kHeldNotMain);
    }
    {
        auto in = Gameplay();
        in.LayerWorld = false;
        CHECK(!DecideOwner(in).Vr && (DecideOwner(in).Held & kHeldLayer) != 0U);
        in.Probing = true;  // Black with a placement waiting for its probes
        const auto d = DecideOwner(in);
        CHECK(d.Vr && d.Probe);
        in.Setting = 0x1C;  // ... but never through a door
        CHECK(!DecideOwner(in).Vr && (DecideOwner(in).Held & kHeldCinemaRow) != 0U);
        in.Setting = 0;
        in.PlayerValid = false;
        CHECK(!DecideOwner(in).Vr);
        // The probes run over every Held row a scene entry or a door can leave behind (the walk-in's
        // 0x20000000, status != 7 after a load, item gets, a subcamera ...), never over a screen row, a
        // missing player, another camera or mode L.
        struct Grant {
            uint32_t Flags1;
            int16_t Status, Scene, ActiveCamera, Setting;
            uint32_t Held;
        };
        const Grant grants[] = {
            {0x20000000U, 7, 1, 0, 0, kHeldCutscene}, {0, 3, 1, 0, 0, kHeldStatus},
            {0x400U, 7, 1, 0, 0, kHeldItem},          {0x10000000U, 7, 1, 0, 0, kHeldItem},
            {0, 7, 1, 2, 0, kHeldSecondaryCamera},    {0x20U, 7, 1, 0, 0, kHeldState},
            {0x200U, 7, 1, 0, 0, kHeldState},         {0, 7, 1, 0, 0x14, kHeldSetting},
            {0, 7, 0x45, 0, 0, kHeldScene},
        };
        for (const auto& grant : grants) {
            auto probe = Gameplay();
            probe.LayerWorld = false;
            probe.Probing = true;
            probe.Flags1 = grant.Flags1;
            probe.Status = grant.Status;
            probe.Scene = grant.Scene;
            probe.ActiveCamera = grant.ActiveCamera;
            probe.Setting = grant.Setting;
            const auto g = DecideOwner(probe);
            CHECK(g.Vr && g.Probe && g.Held == 0U && g.ProbeHeld == grant.Held);
            probe.Probing = false;  // the same row without a pending placement: native
            CHECK(!DecideOwner(probe).Vr && (DecideOwner(probe).Held & grant.Held) != 0U);
        }
        auto blocked = Gameplay();
        blocked.LayerWorld = false;
        blocked.Probing = true;
        CHECK(DecideOwner(blocked).Probe && DecideOwner(blocked).ProbeHeld == 0U);
        blocked.ForceHeld = true;
        CHECK(!DecideOwner(blocked).Vr && (DecideOwner(blocked).Held & kHeldConfig) != 0U);
        blocked.ForceHeld = false;
        blocked.Flags1 = 0x00100000U;  // first person
        CHECK(!DecideOwner(blocked).Vr);
        blocked.Flags1 = 0x20000000U;
        blocked.MainCamera = false;
        CHECK(!DecideOwner(blocked).Vr && DecideOwner(blocked).Held == kHeldNotMain);
    }
    {
        auto in = Gameplay();
        in.HeadValid = false;
        CHECK(!DecideOwner(in).Vr && DecideOwner(in).Held == kHeldNoHead);
        in = Gameplay();
        in.ForceHeld = true;  // mode L
        CHECK(!DecideOwner(in).Vr && DecideOwner(in).Held == kHeldConfig);
    }
    // Held rows (native camera, rig live) and Held-static rows (rig frozen, motion test).
    struct Row {
        uint32_t Flags1;
        uint32_t Flags2;
        int16_t Setting;
        int16_t Status;
        int16_t Scene;
        int16_t ActiveCamera;
        int32_t Cutscene;
        int32_t GameMode;
        uint32_t Held;
        bool Static;
    };
    const Row rows[] = {
        {0, 0, 0x14, 7, 1, 0, 0, 0, kHeldSetting, false},
        {0, 0, 0x15, 7, 1, 0, 0, 0, kHeldSetting, false},
        {0, 0, 0x23, 7, 1, 0, 0, 0, kHeldSetting, false},
        {0, 0, 0x40, 7, 1, 0, 0, 0, kHeldSetting, false},
        {0, 0, 0x46, 7, 1, 0, 0, 0, kHeldSetting, false},
        {0, 0, 0, 7, 0x45, 0, 0, 0, kHeldScene, false},
        {0, 0, 0, 1, 1, 0, 0, 0, kHeldStatus, false},
        {0x20, 0, 0, 7, 1, 0, 0, 0, kHeldState, false},
        {0x200, 0, 0, 7, 1, 0, 0, 0, kHeldState, false},
        {0, 0, 0, 7, 1, 1, 0, 0, kHeldSecondaryCamera, true},
        {0x20000000U, 0, 0, 7, 1, 0, 0, 0, kHeldCutscene, true},
        {0x400, 0, 0, 7, 1, 0, 0, 0, kHeldItem, true},
        {0x10000000U, 0, 0, 7, 1, 0, 0, 0, kHeldItem, true},
        {0x100000U, 0, 0, 7, 1, 0, 0, 0, kHeldCinemaRow, false},  // first person
        {0x800000U, 0, 0, 7, 1, 0, 0, 0, kHeldCinemaRow, false},  // Epona
        {0, 0x40000U, 0, 7, 1, 0, 0, 0, kHeldCinemaRow, false},   // crawling
        {0, 0, 0x16, 7, 1, 0, 0, 0, kHeldCinemaRow, false},
        {0, 0, 0x19, 7, 1, 0, 0, 0, kHeldCinemaRow, false},
        {0, 0, 0x1B, 7, 1, 0, 0, 0, kHeldCinemaRow, false},
        {0, 0, 0x1D, 7, 1, 0, 0, 0, kHeldCinemaRow, false},  // a door (TopScreen's 0x1D block is the door row)
        {0, 0, 0x2F, 7, 1, 0, 0, 0, kHeldCinemaRow, false},
        {0, 0, 0, 7, 1, 0, 0xFFF0, 0, kHeldCinemaRow, false},
        {0, 0, 0, 7, 1, 0, 0, 1, kHeldCinemaRow, false},
    };
    for (const auto& row : rows) {
        auto in = Gameplay();
        in.Flags1 = row.Flags1;
        in.Flags2 = row.Flags2;
        in.Setting = row.Setting;
        in.Status = row.Status;
        in.Scene = row.Scene;
        in.ActiveCamera = row.ActiveCamera;
        in.CutsceneIndex = row.Cutscene;
        in.GameMode = row.GameMode;
        const auto d = DecideOwner(in);
        CHECK(!d.Vr && d.Held == row.Held);
        CHECK(d.HeldStatic == row.Static && (!row.Static || d.RigFrozen));
    }
    {
        // Talking and shop browsing stay host-owned with the tether frozen and the stick unrotated.
        auto in = Gameplay();
        in.Flags1 = kFlags1Talking;
        auto d = DecideOwner(in);
        CHECK(d.Vr && d.Talking && d.RigFrozen && d.StickBypass && !d.HeldStatic);
        in.Flags1 = 0;
        in.Setting = kSettingShop;
        d = DecideOwner(in);
        CHECK(d.Vr && d.Shop && d.RigFrozen && d.StickBypass);
        // Talking during an item cutscene: native, static, but the motion test never runs while talking
        // (the session gates it on !Talking).
        in.Setting = 0;
        in.Flags1 = kFlags1Talking | 0x10000000U;
        d = DecideOwner(in);
        CHECK(!d.Vr && d.Talking && d.HeldStatic);
    }
    {
        // Z-target bits are carved out of TopScreen's mask: host-owned, stick unbypassed.
        for (uint32_t bit : {0x10U, 0x8000U, 0x10000U, 0x20000U}) {
            auto in = Gameplay();
            in.Flags1 = bit;
            const auto d = DecideOwner(in);
            CHECK(d.Vr && d.ZTarget && !d.StickBypass);
        }
        // Every other bit of TopScreen's flags1 mask 0x20938230 hands the camera back.
        constexpr uint32_t kMask = 0x20938230U;
        for (uint32_t bit = 1U; bit != 0U; bit <<= 1U) {
            if ((kMask & bit) == 0U || (kFlags1ZTargetMask & bit) != 0U) continue;
            auto in = Gameplay();
            in.Flags1 = bit;
            CHECK(!DecideOwner(in).Vr);
        }
        // Swimming stays in the world.
        auto in = Gameplay();
        in.Flags1 = 0x08000000U;
        CHECK(DecideOwner(in).Vr);
        // First person bypasses the stick even while native.
        in.Flags1 = kFlags1FirstPerson;
        CHECK(DecideOwner(in).StickBypass);
    }
    CHECK(HeldNames(0) == "none");
    CHECK(HeldNames(kHeldSetting | kHeldStatus) == "setting|status");
    Report("owner");
}

// ---------------------------------------------------------------------------------------------------
void TestCutsceneMonitor() {
    const double s = 33.0;
    {
        CutsceneCameraMonitor m;
        CHECK(!m.Update(false, 0.0, {0, 0, 0}, 0.0, s));
        bool any = false;
        for (int i = 0; i < 60; ++i) any |= m.Update(true, i / 30.0, {0, 0, 0}, 0.0, s);
        CHECK(!any);  // a still camera never trips
    }
    {
        // A camera that rests settles after 5 still updates (0.2 s): a 3 m move within 1 s then trips; it
        // holds while the row lasts and clears with it.
        CutsceneCameraMonitor m;
        for (int i = 0; i < 6; ++i) CHECK(!m.Update(true, i / 30.0, {0, 0, 0}, 0.0, s));
        CHECK(m.Settled() && m.SettledStill() && Near(m.SettleSeconds(), 5.0 / 30.0, 1e-9));
        bool tripped = false;
        int i = 6;
        for (; i <= 40 && !tripped; ++i) tripped = m.Update(true, i / 30.0, {(i - 5) * 0.1 * s, 0, 0}, 0.0, s);
        CHECK(tripped && m.TripMovedM() > 2.0 && i <= 30);
        CHECK(m.Update(true, 2.0, {0, 0, 0}, 0.0, s));  // the camera stopped: still Cinema
        CHECK(!m.Update(false, 2.1, {0, 0, 0}, 0.0, s));
        CHECK(!m.Settled());
        CHECK(!m.Update(true, 2.2, {0, 0, 0}, 0.0, s));
    }
    {
        // A scripted move that never rests: the window opens 0.75 s after the row began, so 3 m/s trips
        // about 0.7 s after that (and never during the hand-over).
        CutsceneCameraMonitor m;
        bool tripped = false;
        double at = -1.0;
        for (int i = 0; i <= 90 && !tripped; ++i) {
            tripped = m.Update(true, i / 30.0, {i * 0.1 * s, 0, 0}, 0.0, s);
            if (tripped) at = i / 30.0;
        }
        CHECK(tripped && m.Settled() && !m.SettledStill() && Near(m.SettleSeconds(), 0.75, 0.034));
        CHECK(at > 1.3 && at < 1.6);
    }
    {
        // The hand-over: the native camera swings 90 deg and travels 3 m from the head it took over toward its
        // own view (Normal1's lerp, ~0.4 s to settle), then rests. No trip.
        CutsceneCameraMonitor m;
        bool any = false;
        double yaw = 0.0, x = 0.0;
        for (int i = 0; i < 90; ++i) {
            yaw += (Deg(90) - yaw) * 0.25;
            x += (3.0 * s - x) * 0.25;
            any |= m.Update(true, i / 30.0, {x, 0, 0}, yaw, s);
        }
        CHECK(!any && m.Settled());
        // A OnePoint subcamera's first cut (a jump) right after the hand-over, then still: no trip either.
        CutsceneCameraMonitor cut;
        any = false;
        for (int i = 0; i < 60; ++i)
            any |= cut.Update(true, i / 30.0, i < 2 ? M::Vec3{0, 0, 0} : M::Vec3{5 * s, 0, 2 * s}, i < 2 ? 0.0 : Deg(120), s);
        CHECK(!any && cut.SettledStill());
        // ... but a cut after the view settled is a scripted move.
        for (int i = 60; i < 63; ++i) any |= cut.Update(true, i / 30.0, {-3 * s, 0, 0}, Deg(120), s);
        CHECK(any && cut.Tripped());
    }
    {
        // A slow pan (0.5 m/s, 20 deg/s) for 10 s never trips: no 1 s window exceeds the limits.
        CutsceneCameraMonitor m;
        bool any = false;
        for (int i = 0; i < 300; ++i) {
            const double t = i / 30.0;
            any |= m.Update(true, t, {0.5 * s * t, 0, 0}, Deg(20.0 * t), s);
        }
        CHECK(!any && m.Settled());
    }
    {
        // 50 deg in half a second after the view settled trips; across the +-pi seam too.
        CutsceneCameraMonitor m;
        for (int i = 0; i < 6; ++i) m.Update(true, i / 30.0, {0, 0, 0}, Deg(170), s);
        CHECK(m.Settled());
        bool tripped = false;
        for (int i = 1; i <= 15 && !tripped; ++i)
            tripped = m.Update(true, (5 + i) / 30.0, {0, 0, 0}, Deg(170 + i * 50.0 / 15), s);
        CHECK(tripped && m.TripTurnedDegrees() > 45.0 && m.TripTurnedDegrees() < 60.0);
        CutsceneCameraMonitor still;
        bool any = false;
        for (int i = 0; i < 60; ++i) any |= still.Update(true, i / 30.0, {0, 0, 0}, i % 2 == 0 ? Deg(179) : Deg(-179), s);
        CHECK(!any && still.Settled());  // a 2 deg wobble across the seam is not a turn
        CutsceneCameraMonitor bad;
        CHECK(!bad.Update(true, 0.0, {NAN, 0, 0}, 0.0, s) && !bad.Settled());
    }
    Report("cutscene_cam");
}

// ---------------------------------------------------------------------------------------------------
void TestGeometry() {
    const double s = 33.0;
    const double floorY = 0.0;
    HeadInGeometry g;
    CHECK(!g.Valid());
    g.Reset({0, 50, 0});
    {
        // A miss: BgCheckInfo backs the end off 1 u toward q.
        const auto seg = g.Segment({10, 50, 0}, {-40, 50, 0});
        CHECK(NearVec(seg.From, {0, 50, 0}) && NearVec(seg.To, {10, 50, 0}));
        const auto step = g.Update({10, 50, 0}, {9, 50, 0}, floorY, s);
        CHECK(step.Clear && step.Fade == 0.0 && NearVec(g.Q(), {10, 50, 0}));
    }
    {
        // A wall at x = 20 between q = 10 and the head at 25: penetration 6 u -> 6 / 9.9.
        const auto step = g.Update({25, 50, 0}, {19, 50, 0}, floorY, s);
        CHECK(!step.Clear && Near(step.PenetrationU, 6.0, 1e-9) && Near(step.Fade, 6.0 / (0.3 * s), 1e-9));
        CHECK(NearVec(g.Q(), {10, 50, 0}));  // q stays outside
        const auto deep = g.Update({40, 50, 0}, {19, 50, 0}, floorY, s);
        CHECK(deep.Fade == 1.0);  // black at 0.3 m
        const auto back = g.Update({15, 50, 0}, {14, 50, 0}, floorY, s);
        CHECK(back.Clear && back.Fade == 0.0 && NearVec(g.Q(), {15, 50, 0}));
    }
    {
        // A wall just beyond the head (the ray runs 8 u past it) is no penetration: no fade beside walls.
        g.Reset({0, 50, 0});
        const auto step = g.Update({10, 50, 0}, {13, 50, 0}, floorY, s);
        CHECK(step.Clear && step.Fade == 0.0);
        const auto at = g.Update({20, 50, 0}, {19.2, 50, 0}, floorY, s);  // the wall 0.8 u past the end
        CHECK(at.Clear);
    }
    {
        // 1 < p <= 5: neither clear nor a fade; q holds.
        g.Reset({0, 50, 0});
        const auto step = g.Update({10, 50, 0}, {7, 50, 0}, floorY, s);
        CHECK(!step.Clear && step.Fade == 0.0 && NearVec(g.Q(), {0, 50, 0}));
    }
    {
        // A zero-length segment carries no information: clear.
        g.Reset({5, 50, 5});
        CHECK(g.Update({5, 50, 5}, {9, 50, 5}, floorY, s).Clear);
        // The head below Link's floor + 0.2 m: black whatever the segment says.
        const auto low = g.Update({5, 0.1 * s, 5}, {5, 0.1 * s + 1.0, 5}, floorY, s);
        CHECK(low.BelowFloor && low.Fade == 1.0);
        // Non-finite collided: treated as clear (no stuck fade from a failed read).
        g.Reset({0, 50, 0});
        CHECK(g.Update({10, 50, 0}, {NAN, 0, 0}, floorY, s).Clear);
    }
    // The scenes below: a house wall at x = 20 (solid beyond), Link's head at x = -40 in the open.
    const M::Vec3 link{-40, 50, 0};
    // BgCheckInfo against that wall: a crossing segment ends 1 u out of the wall on the side it came from, a
    // miss 1 u short of its end.
    const auto wall = [](const GeometrySegment& seg) {
        const M::Vec3 d = seg.To - seg.From;
        const double length = M::Length(d);
        if (length < 1e-9) return seg.To;
        const M::Vec3 dir = d * (1.0 / length);
        if ((seg.From.X < 20.0) != (seg.To.X < 20.0)) {
            const double hitX = seg.From.X < 20.0 ? 19.0 : 21.0;
            return seg.From + dir * ((hitX - seg.From.X) / dir.X);
        }
        return seg.To - dir;
    };
    const auto test = [&](HeadInGeometry& geometry, const M::Vec3& head) {
        return geometry.Update(head, wall(geometry.Segment(head, link)), floorY, s);
    };
    {
        // The head inside the wall stays faded on every update: q's test fades, the rescue from Link's head
        // (which also crosses the wall) keeps that fade, alternating.
        HeadInGeometry h;
        h.Reset({10, 50, 0});
        auto step = test(h, {26, 50, 0});
        CHECK(!step.Clear && step.Test == GeometryTest::FromQ && step.Fade > 0.0);
        const double fade = step.Fade;
        for (int i = 0; i < 10; ++i) {
            step = test(h, {26, 50, 0});
            CHECK(!step.Clear && Near(step.Fade, fade, 1e-12));
            CHECK(step.Test == (i % 2 == 0 ? GeometryTest::FromLink : GeometryTest::FromQ));
        }
        CHECK(NearVec(h.Q(), {10, 50, 0}));
        // A snap turn pivots on the head: nothing is reset, the head is still inside, still faded.
        step = test(h, {26, 50, 0});
        CHECK(step.Fade > 0.0);
        // Stepping back out clears it.
        step = test(h, {15, 50, 0});
        CHECK(step.Clear && step.Fade == 0.0 && NearVec(h.Q(), {15, 50, 0}));
    }
    {
        // A stale q (the head walked around a pillar while the host did not own the camera): q -> head
        // crosses geometry once, the rescue from Link's head is clear next update and clears the fade.
        HeadInGeometry h;
        h.Reset({30, 50, 0});  // q on the far side of the wall at x = 20 ...
        auto step = test(h, {10, 50, 0});  // ... the head back in Link's open space
        CHECK(!step.Clear && step.Fade > 0.0);
        step = test(h, {10, 50, 0});
        CHECK(step.Clear && step.Test == GeometryTest::FromLink && step.Fade == 0.0 && NearVec(h.Q(), {10, 50, 0}));
        // With the soft reseed (the session marks the first owned update after Held ones) the first test is
        // already Link's: no dark update at all.
        h.Reset({30, 50, 0});
        h.ReseedSoft();
        CHECK(h.PendingReseed() == GeometryReseed::Soft);
        const auto seg = h.Segment({10, 50, 0}, link);
        CHECK(NearVec(seg.From, link) && NearVec(seg.To, {10, 50, 0}));
        step = h.Update({10, 50, 0}, wall(seg), floorY, s);
        CHECK(step.Clear && step.Reseed == GeometryReseed::Soft && step.Fade == 0.0 && !step.ReseedHit);
        CHECK(h.PendingReseed() == GeometryReseed::None);
    }
    {
        // A soft reseed that hits keeps the last fade (0 here) and hands back to q next update.
        HeadInGeometry h;
        h.Reset({10, 50, 0});
        h.ReseedSoft();
        auto step = test(h, {26, 50, 0});
        CHECK(!step.Clear && !step.ReseedHit && step.Fade == 0.0 && step.Test == GeometryTest::FromLink);
        step = test(h, {26, 50, 0});
        CHECK(step.Test == GeometryTest::FromQ && step.Fade > 0.0);
        // ... and a fade held over Held updates survives a soft reseed that hits.
        h.ReseedSoft();
        const double held = h.LastFade();
        step = test(h, {26, 50, 0});
        CHECK(step.Test == GeometryTest::FromLink && Near(step.Fade, held, 1e-12) && held > 0.0);
    }
    {
        // A blink: the hard reseed tests from Link's head. Clear: q = the head. A hit: ReseedHit (the
        // session re-places) with the depth shown meanwhile.
        HeadInGeometry h;
        h.Reset({-100, 50, 0});
        h.ReseedHard();
        auto step = test(h, {0, 50, 0});
        CHECK(step.Clear && step.Reseed == GeometryReseed::Hard && !step.ReseedHit && NearVec(h.Q(), {0, 50, 0}));
        h.ReseedHard();
        step = test(h, {30, 50, 0});  // landed 10 u inside the wall
        CHECK(!step.Clear && step.ReseedHit && step.Reseed == GeometryReseed::Hard && step.Fade > 0.0);
        CHECK(h.PendingReseed() == GeometryReseed::None);
        // A hard reseed overrides a pending soft one; a soft one never downgrades a hard one.
        h.ReseedSoft();
        h.ReseedHard();
        CHECK(h.PendingReseed() == GeometryReseed::Hard);
        h.ReseedSoft();
        CHECK(h.PendingReseed() == GeometryReseed::Hard);
        // A placement clears everything.
        h.Reset({0, 50, 0});
        CHECK(h.PendingReseed() == GeometryReseed::None && h.LastFade() == 0.0);
    }
    {
        // Below Link's floor wins over a clear rescue; a non-finite Link head falls back to q.
        HeadInGeometry h;
        h.Reset({0, 50, 0});
        h.ReseedSoft();
        auto step = test(h, {0, 2, 0});
        CHECK(step.BelowFloor && step.Fade == 1.0);
        h.Reset({0, 50, 0});
        h.ReseedHard();
        const auto seg = h.Segment({5, 50, 0}, {NAN, 0, 0});
        CHECK(NearVec(seg.From, {0, 50, 0}));
        step = h.Update({5, 50, 0}, wall(seg), floorY, s);
        CHECK(step.Clear && step.Test == GeometryTest::FromQ);
        CHECK(std::string_view(GeometryTestName(GeometryTest::FromLink)) == "link" &&
              std::string_view(GeometryReseedName(GeometryReseed::Hard)) == "hard");
    }
    Report("geometry");
}

// ---------------------------------------------------------------------------------------------------
// The collided end BgCheckInfo returns for a probe with a wall at `wallM` (a miss: the end backed off 1 u).
M::Vec3 ProbeEnd(const ProbeRequest& r, double wallM, double s) {
    const M::Vec3 d = r.To - r.From;
    const double length = M::Length(d);
    const M::Vec3 dir = d * (1.0 / length);
    if (wallM * s >= length) return r.To - dir;
    return r.From + dir * (wallM * s - 1.0);
}

ProbeChoice RunProbes(const double walls[5], double facing, double s, int* requests) {
    PlacementProbes p;
    p.Start(2.3);
    const M::Vec3 head{0, 50, 0};
    ProbeRequest r;
    *requests = 0;
    while (!p.Done() && p.Next(head, facing, s, &r)) {
        ++*requests;
        p.Feed(r.Index, r.From, ProbeEnd(r, walls[r.Index], s), s);
    }
    return p.Choose();
}

void TestProbes() {
    const double s = 33.0;
    {
        PlacementProbes p;
        p.Start(2.3);
        ProbeRequest r;
        CHECK(p.Next({0, 50, 0}, 0.0, s, &r) && r.Index == 0);
        CHECK(NearVec(r.To, {0, 50, -2.3 * s}, 1e-9));  // behind Link facing +Z
        p.Feed(0, r.From, ProbeEnd(r, 10.0, s), s);
        CHECK(p.Next({0, 50, 0}, 0.0, s, &r) && r.Index == 1);
        // back_left = behind turned toward Link's left (+X when he faces +Z).
        CHECK(r.To.X > 0.0 && r.To.Z < 0.0);
        CHECK(Near(M::YawOf(r.To - r.From), Deg(180 - 35), 1e-9));
        CHECK(!p.Done());
    }
    {
        const double walls[5] = {10, 10, 10, 10, 10};
        int requests = 0;
        const auto c = RunProbes(walls, 0.0, s, &requests);
        CHECK(requests == 3 && c.Probed && !c.Fallback && c.Index == 0 && Near(c.DistanceM, 2.0, 1e-9));
        CHECK(std::string_view(c.Name) == "back" && Near(c.OffsetYaw, 0.0));
    }
    {
        const double walls[5] = {0.5, 10, 1.2, 10, 10};
        int requests = 0;
        const auto c = RunProbes(walls, 0.0, s, &requests);
        CHECK(requests == 3 && c.Index == 1 && std::string_view(c.Name) == "back_left" &&
              Near(c.DistanceM, 2.0, 1e-9) && Near(c.OffsetYaw, Deg(-35), 1e-12));
        CHECK(Near(c.ClearM[0], 0.5 - 1.0 / s, 1e-6) && Near(c.ClearM[1], 2.3, 1e-9));
    }
    {
        // Behind blocked (< 1.0 m clear): left and right are probed too.
        const double walls[5] = {0.6, 0.8, 0.9, 1.5, 0.4};
        int requests = 0;
        const auto c = RunProbes(walls, Deg(30), s, &requests);
        CHECK(requests == 5 && std::string_view(c.Name) == "left" && Near(c.DistanceM, 1.2 - 1.0 / s, 1e-6));
    }
    {
        const double walls[5] = {0.3, 0.3, 0.3, 0.3, 0.3};
        int requests = 0;
        const auto c = RunProbes(walls, 0.0, s, &requests);
        CHECK(requests == 5 && c.Fallback && Near(c.DistanceM, 0.7) && Near(c.OffsetYaw, 0.0));
    }
    {
        PlacementProbes p;
        CHECK(!p.Active() && !p.Choose().Probed);
        p.Start(2.3);
        CHECK(p.Active() && !p.Choose().Probed && p.Count() == 0);
        p.Cancel();
        ProbeRequest r;
        CHECK(!p.Next({0, 0, 0}, 0.0, s, &r));
    }
    {
        // The probe choice through SolvePlacement: Jamal lands along the probed direction from Link, at the
        // probed distance (unmirrored and mirrored).
        for (bool mirrored : {false, true}) {
            for (double offset : {0.0, -35.0, 35.0, 90.0}) {
                ProbeChoice c;
                c.Probed = true;
                c.OffsetYaw = Deg(offset);
                c.DistanceM = 1.2;
                const double facingGame = Deg(70);
                const M::Vec3 linkGame{300, 20, -40};
                M::PlacementInput in;
                in.LinkWorld = M::ToVrWorld(linkGame, mirrored);
                in.LinkFacingYaw = PlacementFacingYaw(facingGame, c, mirrored);
                in.ProbeDistanceM = c.DistanceM;
                in.Head.Position = {0.3, 1.6, 1.8};
                in.CentreT = {0, 0, 0};
                M::PlacementConfig config;
                const auto result = M::SolvePlacement(in, config);
                CHECK(result.Valid && Near(result.DistanceM, 1.2, 1e-9));
                const M::Vec3 headGame = M::ToGameWorld(M::TrackingToWorld(result.NewRig, in.Head.Position), mirrored);
                const M::Vec3 away = headGame - linkGame;
                CHECK(Near(M::LengthXZ(away), 1.2 * config.Scale, 1e-6));
                CHECK(Near(M::WrapAngle(M::YawOf(away) - (facingGame + M::kPi + Deg(offset))), 0.0, 1e-9));
            }
        }
    }
    CHECK(Near(LinkHeadHeightU(false), 50.0) && Near(LinkHeadHeightU(true), 38.0));
    Report("probes");
}

// ---------------------------------------------------------------------------------------------------
// The game's input direction (decomp Player): move yaw = camera yaw + atan2(-x, y).
double MoveYaw(double cameraYaw, int16_t x, int16_t y) {
    return M::WrapAngle(cameraYaw + std::atan2(-static_cast<double>(x), static_cast<double>(y)));
}

void TestStick() {
    {
        // The theta sign test (§7): theta = 90 deg, forward goes 90 deg to the predicted side (left).
        int16_t x = 0, y = 100;
        RotateCirclePad(x, y, Deg(90));
        CHECK(x == -100 && y == 0);
        CHECK(Near(MoveYaw(0.0, x, y), Deg(90), 1e-9));
        x = 100, y = 0;
        RotateCirclePad(x, y, Deg(90));
        CHECK(x == 0 && y == 100);
        x = 154, y = 154;
        RotateCirclePad(x, y, Deg(10));
        CHECK(std::hypot(x, y) <= 154.6 && std::hypot(x, y) >= 153.4);
        x = 0, y = 0;
        RotateCirclePad(x, y, Deg(45));
        CHECK(x == 0 && y == 0);
    }
    {
        // Every stick angle keeps its offset from psi_ref, whatever the camera yaw.
        double worst = 0.0;
        for (double cam : {-3.0, -1.0, 0.0, 0.7, 2.9}) {
            for (double psi : {-2.5, 0.0, 1.1, 3.1}) {
                for (double a : {0.0, 30.0, 90.0, 150.0, -120.0}) {
                    int16_t x = static_cast<int16_t>(std::lround(-150.0 * std::sin(Deg(a))));
                    int16_t y = static_cast<int16_t>(std::lround(150.0 * std::cos(Deg(a))));
                    RotateCirclePad(x, y, StickTheta(psi, cam, false));
                    worst = std::max(worst, std::fabs(M::WrapAngle(MoveYaw(cam, x, y) - (psi + Deg(a)))));
                }
            }
        }
        CHECK(worst < Deg(1.0));
        CHECK(Near(StickTheta(1.0, 0.4, false), 0.6, 1e-12) && Near(StickTheta(1.0, 0.4, true), 1.4, 1e-12));
        CHECK(Near(StickTheta(3.0, -3.0, false), M::WrapAngle(6.0), 1e-12));
    }
    {
        // Head-latched: latch on push, follow with tau 0.5 s, unlatch on release.
        StickReference r;
        StickInput in;
        in.HeadValid = true;
        in.HeadYaw = 0.3;
        in.Dt = 1.0 / 30;
        auto step = r.Update(MoveReference::HeadLatched, in);
        CHECK(!step.Rotate);
        in.StickActive = true;
        step = r.Update(MoveReference::HeadLatched, in);
        CHECK(step.Rotate && step.Latched && Near(step.PsiRef, 0.3));
        in.HeadYaw = 1.3;
        for (int i = 0; i < 15; ++i) step = r.Update(MoveReference::HeadLatched, in);
        CHECK(!step.Latched && Near(step.PsiRef, 0.3 + 1.0 * (1.0 - std::exp(-0.5 / 0.5)), 1e-6));
        in.StickActive = false;
        CHECK(!r.Update(MoveReference::HeadLatched, in).Rotate);
        in.StickActive = true;
        step = r.Update(MoveReference::HeadLatched, in);
        CHECK(step.Latched && Near(step.PsiRef, 1.3));
        // The latch follows across the +-pi seam by the short arc.
        StickReference seam;
        in.HeadYaw = 3.1;
        seam.Update(MoveReference::HeadLatched, in);
        in.HeadYaw = -3.1;
        step = seam.Update(MoveReference::HeadLatched, in);
        CHECK(std::fabs(step.PsiRef) > 3.09);
    }
    {
        // Z-target latches at the press and holds while the head looks around.
        StickReference r;
        StickInput in;
        in.HeadValid = true;
        in.StickActive = true;
        in.Dt = 1.0 / 30;
        in.HeadYaw = 0.5;
        in.ZTarget = true;
        auto step = r.Update(MoveReference::HeadLatched, in);
        CHECK(step.Latched && Near(step.PsiRef, 0.5));
        in.HeadYaw = 2.0;
        for (int i = 0; i < 30; ++i) step = r.Update(MoveReference::HeadLatched, in);
        CHECK(Near(step.PsiRef, 0.5) && step.Rotate);
        in.ZTarget = false;
        step = r.Update(MoveReference::HeadLatched, in);
        CHECK(step.Latched && Near(step.PsiRef, 2.0));
    }
    {
        // Bypass and native: no rotation; head mode follows tau 0.1; line uses Link - head beyond 0.6 m.
        StickReference r;
        StickInput in;
        in.HeadValid = true;
        in.StickActive = true;
        in.HeadYaw = 1.0;
        in.Bypass = true;
        CHECK(!r.Update(MoveReference::HeadLatched, in).Rotate);
        in.Bypass = false;
        CHECK(!r.Update(MoveReference::Native, in).Rotate);
        in.HeadValid = false;
        CHECK(!r.Update(MoveReference::HeadLatched, in).Rotate);
        in.HeadValid = true;
        StickReference head;
        in.Dt = 0.1;
        auto step = head.Update(MoveReference::Head, in);
        CHECK(step.Rotate && Near(step.PsiRef, 1.0));
        in.HeadYaw = 2.0;
        step = head.Update(MoveReference::Head, in);
        CHECK(Near(step.PsiRef, 1.0 + (1.0 - std::exp(-1.0)), 1e-9));
        StickReference line;
        in.LineYaw = -0.4;
        in.LineDistanceM = 1.0;
        CHECK(Near(line.Update(MoveReference::Line, in).PsiRef, -0.4));
        in.LineDistanceM = 0.3;
        CHECK(Near(line.Update(MoveReference::Line, in).PsiRef, 2.0));
    }
    {
        MoveReference m = MoveReference::Native;
        CHECK(ParseMoveReference("head-latched", &m) && m == MoveReference::HeadLatched);
        CHECK(ParseMoveReference("line", &m) && m == MoveReference::Line);
        CHECK(ParseMoveReference("native", &m) && m == MoveReference::Native);
        CHECK(!ParseMoveReference("controller", &m));
        CHECK(StickActive(0, 11) && !StickActive(5, 5) && !StickActive(0, 0));
        CHECK(Near(ApproachAngle(0.0, 1.0, 0.0, 0.5), 0.0) && Near(ApproachAngle(NAN, 1.0, 0.1, 0.5), 1.0));
    }
    Report("stick");
}

// ---------------------------------------------------------------------------------------------------
void TestAlwaysAndPreview() {
    const double s = 33.0;
    CHECK(NearHead({0, 0, 100}, {0, 0, 0}, 4.0 * s));
    CHECK(!NearHead({0, 0, 140}, {0, 0, 0}, 4.0 * s));
    CHECK(!NearHead({NAN, 0, 0}, {0, 0, 0}, 4.0 * s) && !NearHead({0, 0, 0}, {0, 0, 0}, 0.0));
    {
        CircleHeadConfig c;
        const M::Vec3 link{100, 0, 200};
        auto h = CirclePreviewHead(link, 0.0, 50.0, c, 0.0, s);
        // Starts behind Link (he faces +Z), at eye height, looking at his head.
        CHECK(h.Valid && NearVec(h.Position, {100, 1.6 * s, 200 - 1.2 * s}, 1e-9) && Near(h.Yaw, 0.0, 1e-9));
        const auto w = BuildCameraWrite(h);
        CHECK(w.Valid && w.Pitch < 0.0 && w.Pitch > Deg(-10));
        h = CirclePreviewHead(link, 0.0, 50.0, c, c.PeriodSeconds / 4.0, s);
        CHECK(h.Valid && Near(h.Position.X, 100 - 1.2 * s, 1e-6) && Near(h.Yaw, Deg(90), 1e-6));
        h = CirclePreviewHead(link, 0.0, 50.0, c, c.PeriodSeconds / 2.0, s);
        CHECK(Near(h.Position.Z, 200 + 1.2 * s, 1e-6) && Near(std::fabs(h.Yaw), Deg(180), 1e-6));
        CHECK(CircleStep(0.0, 8.0) == 0 && CircleStep(1.01, 8.0) == 1 && CircleStep(7.99, 8.0) == 7 &&
              CircleStep(8.5, 8.0) == 0);
    }
    Report("always_preview");
}

}  // namespace

int main() {
    TestCameraWrite();
    TestOwner();
    TestCutsceneMonitor();
    TestGeometry();
    TestProbes();
    TestStick();
    TestAlwaysAndPreview();
    std::printf("oot3d_xr_camera_logic_tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
