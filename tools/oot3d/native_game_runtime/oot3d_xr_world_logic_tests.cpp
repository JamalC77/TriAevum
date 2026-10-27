// oot3d_xr_world_logic_tests: the pure decisions of the in-world first light (oot3d_xr_world_logic.h;
// quest-research/inworld-spec.md §2.3, §2.7, §3.4, §4.3). Title-free and dependency-free; runs on any
// host (PC, or the headset through adb).
//
// The identity section is the PC-runnable stand-in for the spec's W1 day-1 pixel check (the preview of
// the left eye with HEAD=identity equals the mono picture): it runs the exact rewrite the eye builder
// uses (classification, chain cache, eye rows) on game-like rows of both portrait rotations and checks
// F' == F to 1e-5 relative for world, frame-camera and sky draws.
#include "oot3d_xr_world_logic.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

using namespace Oot3dNativeGame::XrWorld;
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

// Game-like rows: R * [px; py], f2 = (0, 0, A, B), f3 = (0, 0, C, 0).
M::Rows GameRows(double tL, double tR, double tD, double tU, int rotation, double C = -1.0) {
    const double sx = -2.0 * C / (tR - tL), ox = -C * (tR + tL) / (tR - tL);
    const double sy = -2.0 * C / (tU - tD), oy = -C * (tU + tD) / (tU - tD);
    const double px[4] = {sx, 0, ox, 0}, py[4] = {0, sy, oy, 0};
    // rotation 0: identity; 1: [[0, 1], [-1, 0]] (the 3DS top screen); 2: [[0, -1], [1, 0]]; 3: mirror.
    int r[2][2] = {{1, 0}, {0, 1}};
    if (rotation == 1) r[0][0] = 0, r[0][1] = 1, r[1][0] = -1, r[1][1] = 0;
    if (rotation == 2) r[0][0] = 0, r[0][1] = -1, r[1][0] = 1, r[1][1] = 0;
    if (rotation == 3) r[0][0] = -1;
    M::Rows f{};
    for (int k = 0; k < 4; ++k) {
        f[0][k] = static_cast<float>(r[0][0] * px[k] + r[0][1] * py[k]);
        f[1][k] = static_cast<float>(r[1][0] * px[k] + r[1][1] * py[k]);
    }
    f[2] = {0.0F, 0.0F, -1.002F, -20.02F};
    f[3] = {0.0F, 0.0F, static_cast<float>(C), 0.0F};
    return f;
}

std::array<float, 16> CameraAt(double yaw, double pitch, const M::Vec3& eye) {
    // World -> view: inverse of T(eye) * RotY(yaw) * RotX(pitch).
    const M::Mat4 rx = [&] {
        M::Mat4 m = M::Mat4::Identity();
        m.M[1][1] = std::cos(pitch);
        m.M[1][2] = -std::sin(pitch);
        m.M[2][1] = std::sin(pitch);
        m.M[2][2] = std::cos(pitch);
        return m;
    }();
    const M::Mat4 pose = M::Translation(eye) * M::RotationY(yaw) * rx;
    M::Mat4 view;
    M::InverseAffine(pose, &view);
    return M::ToColumnMajor(view);
}

double RowError(const M::Rows& a, const M::Rows& b) {
    double scale = 0.0, error = 0.0;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            scale = std::max(scale, std::fabs(static_cast<double>(b[r][c])));
            error = std::max(error, std::fabs(static_cast<double>(a[r][c]) - b[r][c]));
        }
    return error / std::max(scale, 1e-30);
}

void TestClasses() {
    XrDrawFacts f;
    f.Projection = M::ProjectionStatus::Ok;
    f.HasWorldToView = true;
    CHECK(ClassifyXrDraw(f) == XrDrawClass::World);
    f.OnTopTarget = false;
    CHECK(ClassifyXrDraw(f) == XrDrawClass::OffTarget);
    f.OnTopTarget = true;
    f.UiDomain = true;
    CHECK(ClassifyXrDraw(f) == XrDrawClass::Ui);
    f.ScreenContent = true;
    CHECK(ClassifyXrDraw(f) == XrDrawClass::Screen);
    f = {};
    f.Projection = M::ProjectionStatus::NotPerspective;
    CHECK(ClassifyXrDraw(f) == XrDrawClass::OrthoOmitted);
    f.ViewportCoversTarget = true;
    CHECK(ClassifyXrDraw(f) == XrDrawClass::OrthoFull);
    // A perspective f3 with C >= 0 is the fallback class (§2.2), whatever its viewport: never drawn
    // head-locked with the game's rows. Only a true orthographic f3 is ortho.
    f.Projection = M::ProjectionStatus::NonNegativeC;
    CHECK(ClassifyXrDraw(f) == XrDrawClass::Fallback);
    f.ViewportCoversTarget = false;
    CHECK(ClassifyXrDraw(f) == XrDrawClass::Fallback);
    {
        // From real rows: C = +1 perspective (full and partial viewport), and an orthographic f3.
        const M::Rows positive = GameRows(-0.8, 0.8, -0.5, 0.5, 1, 1.0);
        XrDrawFacts g;
        g.Projection = M::DecomposeGameProjection(positive).Status;
        CHECK(g.Projection == M::ProjectionStatus::NonNegativeC);
        g.HasWorldToView = true;
        g.ViewportCoversTarget = true;
        CHECK(ClassifyXrDraw(g) == XrDrawClass::Fallback);
        g.ViewportCoversTarget = false;
        CHECK(ClassifyXrDraw(g) == XrDrawClass::Fallback);
        M::Rows ortho{};
        ortho[0] = {0.0F, 0.01F, 0.0F, -1.0F};
        ortho[1] = {-0.008F, 0.0F, 0.0F, 1.0F};
        ortho[2] = {0.0F, 0.0F, 0.001F, -0.5F};
        ortho[3] = {0.0F, 0.0F, 0.0F, 1.0F};
        g.Projection = M::DecomposeGameProjection(ortho).Status;
        CHECK(g.Projection == M::ProjectionStatus::NotPerspective);
        CHECK(ClassifyXrDraw(g) == XrDrawClass::OrthoOmitted);
        g.ViewportCoversTarget = true;
        CHECK(ClassifyXrDraw(g) == XrDrawClass::OrthoFull);
    }
    f.ViewportCoversTarget = true;
    f.Projection = M::ProjectionStatus::OffAxis;
    CHECK(ClassifyXrDraw(f) == XrDrawClass::Fallback);
    f.Projection = M::ProjectionStatus::Ok;
    f.HasWorldToView = false;
    CHECK(ClassifyXrDraw(f) == XrDrawClass::Fallback);
    f.HasFrameCamera = true;
    CHECK(ClassifyXrDraw(f) == XrDrawClass::FrameCam);
    f.HasWorldToView = true;
    f.Atmosphere = true;
    f.SkyScope = true;
    CHECK(ClassifyXrDraw(f) == XrDrawClass::World);  // not camera-centred: rain, dust, kankyo
    f.CameraCentred = true;
    CHECK(ClassifyXrDraw(f) == XrDrawClass::Sky);
    f.SkyScope = false;
    CHECK(ClassifyXrDraw(f) == XrDrawClass::World);
    CHECK(TreatmentOf(XrDrawClass::Ui) == XrDrawTreatment::Omit);
    CHECK(TreatmentOf(XrDrawClass::OrthoOmitted) == XrDrawTreatment::Omit);
    CHECK(TreatmentOf(XrDrawClass::Fallback) == XrDrawTreatment::Omit);
    CHECK(TreatmentOf(XrDrawClass::Screen) == XrDrawTreatment::GameUniforms);
    CHECK(TreatmentOf(XrDrawClass::OffTarget) == XrDrawTreatment::GameUniforms);
    CHECK(TreatmentOf(XrDrawClass::OrthoFull) == XrDrawTreatment::GameUniforms);
    CHECK(TreatmentOf(XrDrawClass::World) == XrDrawTreatment::EyeRows);
    CHECK(TreatmentOf(XrDrawClass::FrameCam) == XrDrawTreatment::EyeRows);
    CHECK(TreatmentOf(XrDrawClass::Sky) == XrDrawTreatment::SkyRows);
    for (size_t i = 0; i < kXrDrawClassCount; ++i)
        CHECK(std::string_view(XrDrawClassName(static_cast<XrDrawClass>(i))) != "unknown");

    std::array<float, 4> r0{1, 0, 0, 3}, r1{0, 1, 0, 4}, r2{0, 0, 1, 0};
    double distance = 0;
    CHECK(CameraCentred(r0, r1, r2, 5.0, &distance) && std::fabs(distance - 5.0) < 1e-9);
    CHECK(!CameraCentred(r0, r1, r2, 4.9));

    XrSkyScopes scopes;
    CHECK(scopes.Contains(0x1234));
    scopes.Excluded[scopes.ExcludedCount++] = 0x1234;
    CHECK(!scopes.Contains(0x1234));
    CHECK(scopes.ParseAllowed("0x2D9664, 0x297508"));
    CHECK(scopes.AllowedCount == 2 && scopes.Contains(0x2D9664) && scopes.Contains(0x297508) && !scopes.Contains(5));
    CHECK(!scopes.ParseAllowed("0x2D9664,zz"));
    Report("classes");
}

void TestIdentity() {
    // The left eye at the frame camera with the game's own tangents (§2.2 identity, §8 W1 day 1).
    const double tangents[][4] = {{-0.95, 0.95, -0.58, 0.58}, {-1.2, 0.9, -0.7, 0.5}, {-0.3, 0.6, -0.4, 0.2}};
    const std::array<float, 16> cameras[] = {
        CameraAt(0.3, -0.2, {120.0, 54.0, -300.0}),
        CameraAt(-2.9, 0.45, {-4000.0, 800.0, 2500.0}),
        CameraAt(1.57, 0.0, {0.0, 0.0, 0.0}),
    };
    for (int rotation = 0; rotation < 4; ++rotation) {
        for (const auto& t : tangents) {
            for (const auto& camera : cameras) {
                const M::Rows f = GameRows(t[0], t[1], t[2], t[3], rotation);
                const auto game = M::DecomposeGameProjection(f);
                CHECK(game.Ok());
                const M::Mat4 view = M::Mat4FromColumnMajor(camera);
                const bool mirrored = M::MirroredFrame(game, view);
                XrEyeInput eye;
                eye.Valid = true;
                eye.View = view;
                eye.Fov = game.Tangents;
                CHECK(mirrored == (rotation == 3));
                if (mirrored) {
                    // The equivalent eye of a mirrored frame (§2.2): V_i = S * V_g * M with the view-space
                    // mirror S = diag(-1, 1, 1) and mirrored x tangents; a proper rigid eye (det +1).
                    eye.View = M::MirrorMatrix(true) * view * M::MirrorMatrix(true);
                    eye.Fov.TanLeft = -game.Tangents.TanRight;
                    eye.Fov.TanRight = -game.Tangents.TanLeft;
                    CHECK(M::Determinant3(eye.View) > 0.0);
                }
                XrChainCache cache;
                cache.Reset(eye, mirrored);
                // World, frame-camera and sky draws through the same camera.
                for (bool sky : {false, true, false}) {
                    M::Rows out{};
                    CHECK(XrDrawRows(f, camera, sky, cache, &out));
                    CHECK(RowError(out, f) <= 1e-5);
                }
                // A second draw with other x/y rows but the same camera reuses the chain; the eye's
                // tangents replace its x/y rows, its own z/w rows stay (here equal to f's).
                const M::Rows g = GameRows(t[0] * 0.5, t[1] * 0.5, t[2] * 0.5, t[3] * 0.5, rotation);
                M::Rows out{};
                CHECK(XrDrawRows(g, camera, false, cache, &out));
                CHECK(RowError(out, f) <= 1e-5);
                CHECK(cache.Hits() >= 2);
            }
        }
    }
    // A singular camera falls back; an invalid eye builds nothing.
    XrEyeInput eye;
    eye.Valid = true;
    XrChainCache cache;
    cache.Reset(eye, false);
    std::array<float, 16> singular{};
    M::Rows out{};
    CHECK(!XrDrawRows(GameRows(-1, 1, -1, 1, 1), singular, false, cache, &out));
    CHECK(!XrDrawRows(GameRows(-1, 1, -1, 1, 1), singular, false, cache, &out));  // cached failure
    eye.Valid = false;
    cache.Reset(eye, false);
    CHECK(!XrDrawRows(GameRows(-1, 1, -1, 1, 1), CameraAt(0, 0, {}), false, cache, &out));
    Report("identity");
}

void TestPreviewHead() {
    XrPreviewHead head;
    CHECK(head.Parse("identity") && head.Kind == XrPreviewHeadKind::Identity);
    CHECK(head.Parse("sweep90") && head.Kind == XrPreviewHeadKind::Sweep90);
    CHECK(head.Parse("sweep180") && head.Kind == XrPreviewHeadKind::Sweep180);
    CHECK(head.Parse("30,0.1,-0.2,0.5") && head.Kind == XrPreviewHeadKind::Offset && head.YawDegrees == 30.0 &&
          head.X == 0.1 && head.Y == -0.2 && head.Z == 0.5);
    CHECK(!head.Parse("30,0.1"));
    CHECK(!head.Parse("left"));
    head.Parse("sweep90");
    CHECK(std::fabs(head.YawAt(0.0) + 90.0) < 1e-9 && std::fabs(head.YawAt(2.0)) < 1e-9 &&
          std::fabs(head.YawAt(4.0) - 90.0) < 1e-9 && std::fabs(head.YawAt(6.0)) < 1e-9);
    head.Parse("sweep180");
    CHECK(std::fabs(head.YawAt(4.0) - 180.0) < 1e-9);
    // No yaw, no offset, no IPD: V_i == V_g.
    const M::Mat4 view = M::Mat4FromColumnMajor(CameraAt(0.7, 0.1, {10, 20, 30}));
    CHECK(M::MaxAbsDifference(PreviewEyeView(view, 0.0, {}, 0.0), view) < 1e-9);
    // Moving the eye right by d moves a point straight ahead to the left in the eye: x_eye = -d.
    const M::Mat4 shifted = PreviewEyeView(M::Mat4::Identity(), 0.0, {}, 2.0);
    const M::Vec3 p = M::TransformPoint(shifted, {0, 0, -10});
    CHECK(std::fabs(p.X + 2.0) < 1e-9 && std::fabs(p.Z + 10.0) < 1e-9);
    // Yaw 90 (left turn): a point on the camera's left (-X) comes to the eye's front (-Z).
    const M::Mat4 turned = PreviewEyeView(M::Mat4::Identity(), 90.0, {}, 0.0);
    const M::Vec3 q = M::TransformPoint(turned, {-10, 0, 0});
    CHECK(std::fabs(q.Z + 10.0) < 1e-9 && std::fabs(q.X) < 1e-9);
    Report("preview");
}

void TestPacing() {
    XrTickPacer pacer;
    pacer.SetDivisor(3);
    auto step = pacer.Next(100);
    CHECK(step.Started && step.PaceTick == 103 && step.Delta == 3);
    // On time: the guest frame starts at p_{N-1} (+ a little).
    step = pacer.Next(103);
    CHECK(!step.Overrun && step.PaceTick == 106 && step.Delta == 3);
    step = pacer.Next(107);  // one tick late still fits (107 + 2 = 109 = p_N)
    CHECK(!step.Overrun && step.PaceTick == 109);
    step = pacer.Next(108);  // 108 + 2 > 112? no
    CHECK(!step.Overrun && step.PaceTick == 112);
    step = pacer.Next(111);  // 111 + 2 = 113 > 115? no
    CHECK(!step.Overrun && step.PaceTick == 115);
    step = pacer.Next(114);  // 114 + 2 = 116 > 118? no -> 118
    CHECK(!step.Overrun && step.PaceTick == 118);
    step = pacer.Next(120);  // 120 + 2 = 122 > 121: phase reset to 123, a 5-tick frame
    CHECK(step.Overrun && step.PaceTick == 123 && step.Delta == 5);
    step = pacer.Next(123);
    CHECK(!step.Overrun && step.PaceTick == 126 && step.Delta == 3);
    pacer.Restart();
    step = pacer.Next(500);
    CHECK(step.Started && step.PaceTick == 503);
    // Pipelined steady state: the guest one frame ahead of the render worker's tick wait (latest = p_{N-2})
    // is normal, never a resync.
    {
        XrTickPacer p;
        p.SetDivisor(3);
        p.Next(1000);  // 1003
        bool resynced = false;
        uint64_t latest = 1000;
        for (int i = 0; i < 50; ++i) {
            const auto s = p.Next(latest);
            resynced = resynced || s.Resynced || s.Overrun;
            latest = s.PaceTick - 3;  // the previous frame's tick reached
        }
        CHECK(!resynced);
    }
    // A frozen display clock (a held guest, tick waits timing out): the pace never leads it by more
    // than 2d + margin, and each resynced step is one guest frame of presentation time.
    {
        XrTickPacer p;
        p.SetDivisor(3);
        bool bounded = true, oneFrame = true;
        int resyncs = 0;
        for (int i = 0; i < 100; ++i) {
            const auto s = p.Next(2000);
            bounded = bounded && s.PaceTick <= 2000 + p.Divisor() + XrTickPacer::kMargin + p.Divisor();
            oneFrame = oneFrame && s.Delta == 3;
            resyncs += s.Resynced ? 1 : 0;
        }
        CHECK(bounded && oneFrame && resyncs > 0);
        CHECK(p.AheadLimit() == 8U);
        // Far ahead (the reviewed failure: ~1000 ticks after a preparation screen): one step resyncs.
        XrTickPacer q;
        q.SetDivisor(3);
        q.Next(5000);  // 5003
        for (int i = 0; i < 400; ++i) (void)q.Next(5000 + static_cast<uint64_t>(i) * 3);  // keeps pace
        const uint64_t ahead = q.Previous();
        const auto s = q.Next(ahead - 600);  // the clock is 600 ticks behind the pace
        CHECK(s.Resynced && s.PaceTick == ahead - 600 + 3 && s.Delta == 3 && !s.Overrun);
        const auto t = q.Next(ahead - 600 + 3);
        CHECK(!t.Resynced && !t.Overrun && t.PaceTick == ahead - 600 + 6 && t.Delta == 3);
    }
    CHECK(TickDivisor(90.0) == 3 && TickDivisor(72.0) == 2 && TickDivisor(120.0) == 4 && TickDivisor(60.0) == 2 &&
          TickDivisor(0.0) == 3 && TickDivisor(80.0) == 2);
    CHECK(TickDisplayTimeNs(10, 1'000'000'000, 11'111'111, 13) == 1'000'000'000 + 3 * 11'111'111);
    CHECK(TickDisplayTimeNs(10, 1'000'000'000, 11'111'111, 9) == 1'000'000'000 - 11'111'111);
    Report("pacing");
}

XrWorldSignals Gameplay(double now) {
    XrWorldSignals s;
    s.Now = now;
    s.Scene = 0x55;
    s.Entrance = 0xEE;
    s.Link = {100.0, 0.0, 200.0};
    return s;
}

void TestClassifier() {
    XrWorldClassifier c;
    double now = 0.0;
    auto s = Gameplay(now);
    auto step = c.Update(s);
    CHECK(step.State == XrWorldState::Cinema && !step.Changed);
    // 0.5 s clear -> Black (first placement) -> after 0.15 s World with Place.
    bool placed = false;
    const char* reason = "";
    for (int i = 1; i <= 30 && c.State() != XrWorldState::World; ++i) {
        now = i / 30.0;
        step = c.Update(Gameplay(now));
        if (step.Place) {
            placed = true;
            reason = step.PlaceReason;
        }
    }
    CHECK(c.State() == XrWorldState::World && placed && std::string_view(reason) == "first");
    CHECK(now >= 0.5 + 0.15 - 1e-9 && now < 0.8);
    // Text box: non-HUD Ui draws for 0.3 s -> Cinema; a shorter flash does not.
    s = Gameplay(now += 1.0 / 30);
    s.UiDraws = true;
    for (int i = 0; i < 5; ++i) {
        s.Now = now += 1.0 / 30;
        step = c.Update(s);
    }
    CHECK(c.State() == XrWorldState::World);
    for (int i = 0; i < 6; ++i) {
        s.Now = now += 1.0 / 30;
        step = c.Update(s);
    }
    CHECK(c.State() == XrWorldState::Cinema && (step.Blockers & kXrBlockUi) != 0U);
    // The rule can be switched off.
    {
        XrWorldClassifier d;
        auto t = Gameplay(0.0);
        t.UiDraws = true;
        t.UiRuleEnabled = false;
        for (int i = 0; i < 40; ++i) {
            t.Now = i / 30.0;
            d.Update(t);
        }
        CHECK(d.State() == XrWorldState::World);
    }
    // Text gone -> 0.5 s -> Black -> World, placed as a Cinema exit.
    placed = false;
    for (int i = 0; i < 40 && c.State() != XrWorldState::World; ++i) {
        step = c.Update(Gameplay(now += 1.0 / 30));
        if (step.Place) {
            placed = true;
            reason = step.PlaceReason;
        }
    }
    CHECK(c.State() == XrWorldState::World && placed && std::string_view(reason) == "cinema_exit");
    // A door: Black while the setting lasts, then World placed as "door".
    s = Gameplay(now += 1.0 / 30);
    s.CameraSetting = 0x1C;
    step = c.Update(s);
    CHECK(step.State == XrWorldState::Black && step.Changed);
    for (int i = 0; i < 20; ++i) {
        s.Now = now += 1.0 / 30;
        step = c.Update(s);
    }
    CHECK(c.State() == XrWorldState::Black);
    placed = false;
    s.CameraSetting = 0;
    s.Now = now += 1.0 / 30;
    step = c.Update(s);
    CHECK(step.State == XrWorldState::World && step.Place && std::string_view(step.PlaceReason) == "door");
    // A door that never ends: Cinema after the 3 s cap.
    s.CameraSetting = 0x1D;
    for (int i = 0; i < 100; ++i) {
        s.Now = now += 1.0 / 30;
        c.Update(s);
    }
    CHECK(c.State() == XrWorldState::Cinema);
    s.CameraSetting = 0;
    for (int i = 0; i < 40 && c.State() != XrWorldState::World; ++i) {
        s.Now = now += 1.0 / 30;
        c.Update(s);
    }
    CHECK(c.State() == XrWorldState::World);
    // Scene change and teleport -> Black (placed after 0.15 s).
    s.Scene = 0x56;
    s.Now = now += 1.0 / 30;
    step = c.Update(s);
    CHECK(step.State == XrWorldState::Black && (step.Blockers & kXrBlackScene) != 0U);
    for (int i = 0; i < 10 && c.State() != XrWorldState::World; ++i) {
        s.Now = now += 1.0 / 30;
        step = c.Update(s);
    }
    CHECK(c.State() == XrWorldState::World && std::string_view(step.PlaceReason) == "scene");
    s.Link.X += 1.6 * M::kDefaultScale;
    s.Now = now += 1.0 / 30;
    step = c.Update(s);
    CHECK(step.State == XrWorldState::Black && (step.Blockers & kXrBlackTeleport) != 0U);
    s.Now = now += 1.0 / 30;
    step = c.Update(s);  // a normal step after the teleport is no teleport
    CHECK((step.Blockers & kXrBlackTeleport) == 0U);
    for (int i = 0; i < 10 && c.State() != XrWorldState::World; ++i) {
        s.Now = now += 1.0 / 30;
        c.Update(s);
    }
    CHECK(c.State() == XrWorldState::World);
    // Every Cinema row.
    const auto cinema = [&](auto mutate, uint32_t bit) {
        XrWorldClassifier d;
        auto t = Gameplay(0.0);
        for (int i = 0; i < 30; ++i) {
            t.Now = i / 30.0;
            d.Update(t);
        }
        CHECK(d.State() == XrWorldState::World);
        mutate(t);
        t.Now = 1.1;
        auto r = d.Update(t);
        if (bit == kXrBlockNoPlayer) {
            for (int i = 0; i < 80; ++i) {
                t.Now += 1.0 / 30;
                r = d.Update(t);
            }
        }
        CHECK(d.State() == XrWorldState::Cinema && (r.Blockers & bit) != 0U);
        if (d.State() != XrWorldState::Cinema)
            std::printf("  cinema row %s stayed %s\n", XrBlockerNames(bit).c_str(), XrWorldStateName(d.State()));
    };
    cinema([](XrWorldSignals& t) { t.Gameplay = false; }, kXrBlockGameMode);
    cinema([](XrWorldSignals& t) { t.HostGui = true; }, kXrBlockHostGui);
    cinema([](XrWorldSignals& t) { t.Flags1 = 0x20000000U; }, kXrBlockCutscene);
    cinema([](XrWorldSignals& t) { t.CutsceneIndex = 0xFFF1; }, kXrBlockCutscene);
    cinema([](XrWorldSignals& t) { t.Flags1 = 0x00100000U; }, kXrBlockFirstPerson);
    cinema([](XrWorldSignals& t) { t.Flags2 = 0x00040000U; }, kXrBlockCrawl);
    cinema([](XrWorldSignals& t) { t.CameraSetting = 0x1E; }, kXrBlockCrawl);
    cinema([](XrWorldSignals& t) { t.CameraSetting = 0x1A; }, kXrBlockFixedCamera);
    cinema([](XrWorldSignals& t) { t.Flags1 = 0x00800000U; }, kXrBlockEpona);
    cinema([](XrWorldSignals& t) { t.PlayerValid = false; }, kXrBlockNoPlayer);
    cinema([](XrWorldSignals& t) { t.RenderSafety = true; }, kXrBlockRenderSafety);
    cinema([](XrWorldSignals& t) { t.SessionReady = false; }, kXrBlockSession);
    // Held rows stay World (W1) and are reported for the rig freeze.
    {
        XrWorldClassifier d;
        auto t = Gameplay(0.0);
        for (int i = 0; i < 30; ++i) {
            t.Now = i / 30.0;
            d.Update(t);
        }
        t.Flags1 = 0x40U;  // talking
        t.ActiveCamera = 2;
        t.Now = 1.1;
        const auto r = d.Update(t);
        CHECK(d.State() == XrWorldState::World && (r.Blockers & kXrHoldRig) != 0U);
        t.Flags1 = 0x400U;  // item get
        t.Now += 1.0 / 30;
        CHECK((d.Update(t).Blockers & kXrHoldRig) != 0U && d.State() == XrWorldState::World);
    }
    CHECK(XrBlockerNames(0) == "none");
    CHECK(XrBlockerNames(kXrBlockUi | kXrBlackDoor) == "ui|door");
    Report("classifier");
}

void TestRenderSafety() {
    // The hold: tripped -> blocking for 5 s -> cleared; a repeat holds 10 s, then 20, 40, 60, 60.
    {
        XrRenderSafetyHold hold;
        auto s = hold.Update(false, 0.0, false);
        CHECK(!s.Blocking && !s.Tripped && !s.Cleared);
        s = hold.Update(true, 1.0, false);
        CHECK(s.Blocking && s.Tripped && s.HoldSeconds == 5.0);
        s = hold.Update(true, 5.9, false);
        CHECK(s.Blocking && !s.Tripped && !s.Cleared);
        s = hold.Update(true, 6.0, false);  // the stale flag at the clearing update does not block
        CHECK(!s.Blocking && s.Cleared && std::string_view(s.ClearReason) == "hold" && s.HoldSeconds == 5.0);
        s = hold.Update(false, 6.1, false);
        CHECK(!s.Blocking && !s.Cleared);
        const double expected[] = {10.0, 20.0, 40.0, 60.0, 60.0};
        double now = 10.0;
        bool doubling = true;
        for (double seconds : expected) {
            s = hold.Update(true, now, false);
            doubling = doubling && s.Tripped && s.HoldSeconds == seconds;
            s = hold.Update(false, now + seconds - 0.01, false);
            doubling = doubling && s.Blocking;
            s = hold.Update(false, now + seconds, false);
            doubling = doubling && s.Cleared && !s.Blocking;
            now += seconds + 1.0;
        }
        CHECK(doubling);
        // A scene change ends a hold at once and restores the first hold.
        s = hold.Update(true, now, false);
        CHECK(s.Tripped && s.HoldSeconds == 60.0);
        s = hold.Update(false, now + 1.0, true);
        CHECK(s.Cleared && !s.Blocking && std::string_view(s.ClearReason) == "scene");
        s = hold.Update(true, now + 2.0, false);
        CHECK(s.Tripped && s.HoldSeconds == 5.0);
        CHECK(hold.Holding());
    }
    // Through the classifier: World -> safety -> Cinema; the flag clears -> 0.5 s -> Black -> World
    // (placed as a Cinema exit). The reviewed latch: the safety must not pin Cinema for the session.
    {
        XrWorldClassifier c;
        XrRenderSafetyHold hold;
        double now = 0.0;
        auto t = Gameplay(now);
        for (int i = 0; i < 30; ++i) {
            t.Now = now = i / 30.0;
            c.Update(t);
        }
        CHECK(c.State() == XrWorldState::World);
        bool tripped = true;  // the render worker reports 30 fallback frames
        bool sawCinema = false, placedAgain = false;
        const char* reason = "";
        double worldAgainAt = -1.0;
        for (int i = 0; i < 400 && worldAgainAt < 0.0; ++i) {
            t.Now = now += 1.0 / 30;
            const auto safety = hold.Update(tripped, now, false);
            if (safety.Cleared) tripped = false;  // the session clears the reported flag
            t.RenderSafety = safety.Blocking;
            const auto step = c.Update(t);
            if (step.State == XrWorldState::Cinema) sawCinema = true;
            if (step.Place) {
                placedAgain = true;
                reason = step.PlaceReason;
            }
            if (sawCinema && step.State == XrWorldState::World) worldAgainAt = now;
        }
        CHECK(sawCinema && placedAgain && std::string_view(reason) == "cinema_exit");
        // 1.0 s (start) + 5 s hold + 0.5 s clear + 0.15 s black, within a frame or two.
        CHECK(worldAgainAt > 6.6 && worldAgainAt < 6.8);
    }
    Report("safety");
}

// W2 mode R rows of the classifier (§4.3) and the preview circle head (§2.7).
void TestModeR() {
    XrWorldSignals s;
    s.Flags1 = 0x20000000U;  // cutscene
    CHECK((XrCinemaBlockers(s) & kXrBlockCutscene) != 0U);  // W1: the screen
    CHECK((XrHeldRows(s) & kXrHoldRig) == 0U);
    s.ModeR = true;
    CHECK(XrCinemaBlockers(s) == 0U);                  // mode R: stays in the world ...
    CHECK((XrHeldRows(s) & kXrHoldRig) != 0U);         // ... with the rig static
    s.CutsceneCameraMoving = true;                      // until its camera moves
    CHECK((XrCinemaBlockers(s) & kXrBlockCutscene) != 0U);
    s = {};
    s.ModeR = true;
    s.CutsceneIndex = 0xFFF0;
    CHECK((XrCinemaBlockers(s) & kXrBlockCutscene) != 0U);
    s = {};
    s.CameraSetting = 0x17;  // shop browsing: tether frozen in mode R only
    CHECK(XrHeldRows(s) == 0U);
    s.ModeR = true;
    CHECK(XrHeldRows(s) == kXrHoldRig && XrCinemaBlockers(s) == 0U);
    {
        // Through the classifier: a cutscene keeps World in mode R, goes to Cinema once its camera moves.
        XrWorldClassifier c;
        XrWorldSignals t;
        t.ModeR = true;
        double now = 0.0;
        for (int i = 0; i < 60; ++i) {
            t.Now = now = i / 30.0;
            c.Update(t);
        }
        CHECK(c.State() == XrWorldState::World);
        t.Flags1 = 0x20000000U;
        for (int i = 0; i < 30; ++i) {
            t.Now = now += 1.0 / 30;
            const auto step = c.Update(t);
            CHECK(step.State == XrWorldState::World && (step.Blockers & kXrHoldRig) != 0U);
        }
        t.CutsceneCameraMoving = true;
        t.Now = now += 1.0 / 30;
        CHECK(c.Update(t).State == XrWorldState::Cinema);
        // W1 (mode R off): the same flag is the screen at once.
        XrWorldClassifier w1;
        XrWorldSignals u;
        for (int i = 0; i < 60; ++i) {
            u.Now = i / 30.0;
            w1.Update(u);
        }
        u.Flags1 = 0x20000000U;
        u.Now = 2.1;
        CHECK(w1.Update(u).State == XrWorldState::Cinema);
    }
    {
        XrPreviewHead h;
        CHECK(h.Parse("circle:1.2,8") && h.Kind == XrPreviewHeadKind::Circle && h.RadiusM == 1.2 &&
              h.PeriodSeconds == 8.0);
        CHECK(h.YawAt(3.0) == 0.0 && h.X == 0.0 && h.Y == 0.0 && h.Z == 0.0);
        CHECK(h.Parse("circle") && h.Kind == XrPreviewHeadKind::Circle && h.RadiusM == 1.2);
        CHECK(h.Parse("circle:2,16") && h.RadiusM == 2.0 && h.PeriodSeconds == 16.0);
        XrPreviewHead bad;
        CHECK(!bad.Parse("circle:abc") && !bad.Parse("circle:0.1,8") && !bad.Parse("circle:1.2") &&
              !bad.Parse("circle:1.2,8x") && !bad.Parse("circle:1.2,0.5"));
    }
    Report("mode_r");
}

}  // namespace

int main() {
    TestClasses();
    TestIdentity();
    TestPreviewHead();
    TestPacing();
    TestClassifier();
    TestRenderSafety();
    TestModeR();
    std::printf("oot3d_xr_world_logic_tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
