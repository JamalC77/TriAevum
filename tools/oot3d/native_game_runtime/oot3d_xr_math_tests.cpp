// oot3d_xr_math_tests: the pure in-world VR math (oot3d_xr_math.h; quest-research/inworld-spec.md §2.2, §5,
// §8 W0 item 1). Title-free and dependency-free; runs on any host (PC, or the headset through adb).
//
// Covered: projection-row decomposition (Rot3ds, tangents, z/w rows), the identity property with real-
// looking game tangents and both portrait rotations, the four mirror-parity cases (identity through an
// equivalent eye, winding of test triangles, true stereo parallax), rig round trips and V_i, sky rows,
// placement and recenter, leash/glide tether with idle re-centre, floor follow, snap turn, seated offset,
// and numerical robustness.
#include "oot3d_xr_math.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace Oot3dNativeGame::XrMath;

int checks = 0;
int failures = 0;

void Check(bool condition, const char* what, int line) {
    ++checks;
    if (!condition) {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
void CheckNear(double actual, double expected, double epsilon, const char* what, int line) {
    ++checks;
    if (!(std::fabs(actual - expected) <= epsilon)) {
        std::printf("FAIL line %d: %s = %.12g, expected %.12g (eps %.3g)\n", line, what, actual, expected, epsilon);
        ++failures;
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

int sectionChecks = 0;
int sectionFailures = 0;
void Report(const char* section) {
    std::printf("%-14s %4d checks, %d failures\n", section, checks - sectionChecks, failures - sectionFailures);
    sectionChecks = checks;
    sectionFailures = failures;
}
#define CHECK_NEAR(actual, expected, epsilon) CheckNear((actual), (expected), (epsilon), #actual, __LINE__)
// A value that must be at most `limit` (printed on failure).
#define CHECK_LE(value, limit) CheckNear((value) <= (limit) ? 0.0 : (value), 0.0, (limit), #value " <= " #limit, __LINE__)

constexpr double kRowTolerance = 1e-5;  // §2.2: F' == F to 1e-5 relative

struct Rng {
    uint64_t State = 0x9E3779B97F4A7C15ULL;
    double Next() {  // [0, 1)
        State ^= State << 13;
        State ^= State >> 7;
        State ^= State << 17;
        return static_cast<double>(State >> 11) * (1.0 / 9007199254740992.0);
    }
    double Range(double lo, double hi) { return lo + (hi - lo) * Next(); }
};

Vec3 Normalized(const Vec3& v) { return v * (1.0 / Length(v)); }

// The game camera: world -> view looking from `eye` at `target` (RH, -Z forward, +Y up).
Mat4 LookAt(const Vec3& eye, const Vec3& target) {
    const Vec3 forward = Normalized(target - eye);
    const Vec3 side = Normalized(Cross(forward, {0.0, 1.0, 0.0}));
    const Vec3 up = Cross(side, forward);
    Mat4 view = Mat4::Identity();
    view.M[0] = {side.X, side.Y, side.Z, -Dot(side, eye)};
    view.M[1] = {up.X, up.Y, up.Z, -Dot(up, eye)};
    view.M[2] = {-forward.X, -forward.Y, -forward.Z, Dot(forward, eye)};
    return view;
}

Mat4 Inverse(const Mat4& m) {
    Mat4 out = Mat4::Identity();
    const bool ok = InverseAffine(m, &out);
    Check(ok, "test matrix is invertible", __LINE__);
    return out;
}

Rot2 FlipX(const Rot2& r) { return {{{-r[0][0], r[0][1]}, {-r[1][0], r[1][1]}}}; }  // R * diag(-1, 1)

// The two portrait rotations the 3DS top target can carry, then the other det +1 and det -1 permutations.
const Rot2 kPortraitA = {{{0, 1}, {-1, 0}}};
const Rot2 kPortraitB = {{{0, -1}, {1, 0}}};
const Rot2 kAllRotations[8] = {
    {{{0, 1}, {-1, 0}}}, {{{0, -1}, {1, 0}}}, {{{1, 0}, {0, 1}}},  {{{-1, 0}, {0, -1}}},
    {{{-1, 0}, {0, 1}}}, {{{1, 0}, {0, -1}}}, {{{0, 1}, {1, 0}}},  {{{0, -1}, {-1, 0}}},
};

// Game rows from tangents with an arbitrary C < 0 (A's row formula) and the PICA z row for near/far.
Rows RowsFromTangents(const Rot2& r, const EyeFov& t, double c, double nearU = 10.0, double farU = 12800.0) {
    const double dx = t.TanRight - t.TanLeft, dy = t.TanUp - t.TanDown;
    const Vec4 px{-2.0 * c / dx, 0.0, -c * (t.TanRight + t.TanLeft) / dx, 0.0};
    const Vec4 py{0.0, -2.0 * c / dy, -c * (t.TanUp + t.TanDown) / dy, 0.0};
    // PICA clip z in [-w, 0]: z_c / w = -1 at the near plane, 0 at the far plane (w = C z).
    const double a = c * nearU / (farU - nearU);
    const double b = c * nearU * farU / (farU - nearU);
    Rows f{};
    for (int k = 0; k < 4; ++k) {
        f[0][k] = static_cast<float>(r[0][0] * px[k] + r[0][1] * py[k]);
        f[1][k] = static_cast<float>(r[1][0] * px[k] + r[1][1] * py[k]);
    }
    f[2] = {0.0F, 0.0F, static_cast<float>(a), static_cast<float>(b)};
    f[3] = {0.0F, 0.0F, static_cast<float>(c), 0.0F};
    return f;
}

// OoT3D's top screen: 60 deg vertical FOV, 400x240 (5:3); the off-centre variant is a shifted frustum.
EyeFov GameFovCentred() {
    const double tv = std::tan(DegreesToRadians(30.0));
    const double th = tv * 5.0 / 3.0;
    return {-th, th, tv, -tv};
}
EyeFov GameFovOffCentre() { return {-0.93, 1.01, 0.60, -0.55}; }
EyeFov Swapped(const EyeFov& t) { return {-t.TanRight, -t.TanLeft, t.TanUp, t.TanDown}; }

// The largest row-relative difference: |a - b| / max(1, max |b[row]|).
double RowsError(const Rows& a, const Rows& b) {
    double worst = 0.0;
    for (int row = 0; row < 4; ++row) {
        double scale = 1.0;
        for (int k = 0; k < 4; ++k) scale = std::max(scale, std::fabs(static_cast<double>(b[row][k])));
        for (int k = 0; k < 4; ++k)
            worst = std::max(worst, std::fabs(static_cast<double>(a[row][k]) - static_cast<double>(b[row][k])) / scale);
    }
    return worst;
}

Vec4 Clip(const Rows& f, const Vec3& v) {
    Vec4 c{};
    for (int k = 0; k < 4; ++k)
        c[k] = static_cast<double>(f[k][0]) * v.X + static_cast<double>(f[k][1]) * v.Y +
               static_cast<double>(f[k][2]) * v.Z + static_cast<double>(f[k][3]);
    return c;
}
struct Screen {
    double X = 0.0, Y = 0.0;
};
Screen Ndc(const Rows& f, const Vec3& v) {
    const Vec4 c = Clip(f, v);
    return {c[0] / c[3], c[1] / c[3]};
}
// Storage (rotated top target) -> landscape display, undoing a pure rotation: R^T s.
Screen Display(const Rot2& r, const Screen& s) {
    return {r[0][0] * s.X + r[1][0] * s.Y, r[0][1] * s.X + r[1][1] * s.Y};
}
double SignedArea(const Screen& a, const Screen& b, const Screen& c) {
    return (b.X - a.X) * (c.Y - a.Y) - (b.Y - a.Y) * (c.X - a.X);
}

Mat4 RotationX(double angle) { return RotationMatrix(QuatFromAxisAngle({1.0, 0.0, 0.0}, angle)); }
Pose HeadPose(const Vec3& position, double yawDegrees, double pitchDegrees = 0.0) {
    const Quat yaw = QuatFromAxisAngle({0.0, 1.0, 0.0}, DegreesToRadians(yawDegrees));
    const Quat pitch = QuatFromAxisAngle({1.0, 0.0, 0.0}, DegreesToRadians(pitchDegrees));
    return {yaw * pitch, position};
}

// A real-looking Kokiri gameplay camera: behind and above adult Link, looking at his head.
const Vec3 kLinkKokiri{-1480.0, 0.0, 1120.0};
Mat4 KokiriCamera() { return LookAt({-1520.3, 117.5, 1363.7}, {-1480.0, 50.0, 1120.0}); }

// ---------------------------------------------------------------------------------------------------

void TestDecomposition() {
    const EyeFov fov = GameFovCentred();
    for (const Rot2& r : kAllRotations) {
        for (double c : {-1.0, -2.5}) {
            const Rows f = RowsFromTangents(r, fov, c);
            const GameProjection g = DecomposeGameProjection(f);
            CHECK(g.Ok());
            CHECK(g.Rot3ds == r);
            CHECK(g.DetR == Det2(r));
            CHECK_NEAR(g.C, c, 1e-7);
            CHECK_NEAR(g.Tangents.TanLeft, fov.TanLeft, 1e-6);
            CHECK_NEAR(g.Tangents.TanRight, fov.TanRight, 1e-6);
            CHECK_NEAR(g.Tangents.TanUp, fov.TanUp, 1e-6);
            CHECK_NEAR(g.Tangents.TanDown, fov.TanDown, 1e-6);
            CHECK_NEAR(g.Row2[2], f[2][2], 0.0);
            CHECK_NEAR(g.Row2[3], f[2][3], 0.0);
            CHECK_NEAR(g.Row3[2], f[3][2], 0.0);
            CHECK(RotPlus(r)[0][0] * RotPlus(r)[1][1] - RotPlus(r)[0][1] * RotPlus(r)[1][0] == 1);
        }
    }
    {  // portrait A, C = -1: f0 = py, f1 = -px
        const Rows f = RowsFromTangents(kPortraitA, fov, -1.0);
        const GameProjection g = DecomposeGameProjection(f);
        CHECK_NEAR(g.Sx, 1.0 / fov.TanRight, 1e-6);
        CHECK_NEAR(g.Sy, 1.0 / fov.TanUp, 1e-6);
        CHECK_NEAR(g.Ox, 0.0, 1e-7);
        CHECK(f[0][1] > 0.0F && f[1][0] < 0.0F);
        CHECK(g.OffAxis == 0.0);
    }
    {  // off-centre frustum: tangents come back exactly
        const EyeFov off = GameFovOffCentre();
        const GameProjection g = DecomposeGameProjection(RowsFromTangents(kPortraitB, off, -1.0));
        CHECK(g.Ok());
        CHECK_NEAR(g.Tangents.TanLeft, off.TanLeft, 1e-6);
        CHECK_NEAR(g.Tangents.TanRight, off.TanRight, 1e-6);
        CHECK_NEAR(g.Tangents.TanUp, off.TanUp, 1e-6);
        CHECK_NEAR(g.Tangents.TanDown, off.TanDown, 1e-6);
    }
    Report("decomposition");
}

void TestIdentity() {
    // V_i = V_g,d, M = I, eye tangents = the game's own: F' == F, for real-looking game rows and both
    // portrait rotations (plus the other det +1 permutations), centred and off-centre, C = -1 and -2.5.
    const Mat4 camera = KokiriCamera();
    // The production path reads V_g column-major as float: round-trip it first.
    const Mat4 cameraFromFloats = Mat4FromColumnMajor(ToColumnMajor(camera));
    const Mat4 farCamera = LookAt({7950.0, 2100.0, -8040.0}, {7900.0, 1980.0, -7800.0});
    for (const Rot2& r : {kPortraitA, kPortraitB, kAllRotations[2], kAllRotations[3]}) {
        for (const EyeFov& fov : {GameFovCentred(), GameFovOffCentre()}) {
            for (double c : {-1.0, -2.5}) {
                const Rows f = RowsFromTangents(r, fov, c);
                const GameProjection g = DecomposeGameProjection(f);
                for (const Mat4& view : {camera, cameraFromFloats, farCamera}) {
                    Rows out{};
                    CHECK(BuildEyeRows(f, view, view, false, g.Tangents, &out));
                    CHECK_LE(RowsError(out, f), kRowTolerance);
                    // Sky rows at the game camera are the game's rows too.
                    Rows sky{};
                    CHECK(BuildSkyRows(f, view, view, false, g.Tangents, &sky));
                    CHECK_LE(RowsError(sky, f), kRowTolerance);
                }
            }
        }
    }
    Report("identity");
}

// The four mirror-parity cases: det R = +-1 (3DS projection mirror) x det Rot(V_g) = +-1 (view mirror).
void TestMirrorParity() {
    const Mat4 rigidCamera = KokiriCamera();
    const Mat4 mirrorX = MirrorMatrix(true);  // diag(-1, 1, 1): used as a view-space and a world-space mirror
    const double s = kDefaultScale;
    int trianglesTested = 0;
    for (const Rot2& vanilla : {kPortraitA, kPortraitB}) {
        for (bool flipR : {false, true}) {
            for (bool flipV : {false, true}) {
                const Rot2 r = flipR ? FlipX(vanilla) : vanilla;
                const Mat4 vg = flipV ? rigidCamera * mirrorX : rigidCamera;
                const EyeFov gameFov = GameFovOffCentre();
                const Rows f = RowsFromTangents(r, gameFov, -1.0);
                const GameProjection g = DecomposeGameProjection(f);
                CHECK(g.Ok());
                CHECK(g.DetR == (flipR ? -1 : 1));
                CHECK(RotPlus(g.Rot3ds) == vanilla);
                CHECK(AnalyzeView(vg).DetSign == (flipV ? -1 : 1));
                const bool mirrored = MirroredFrame(g, vg);
                CHECK(mirrored == (flipR != flipV));

                // The eye equivalent to the game camera: V_eq * M * V_g^-1 = (flipR ? Mx_view : I), with the
                // tangents swapped when the projection carries the mirror. It is always a proper rigid view.
                const Mat4 veq = (flipR ? mirrorX : Mat4::Identity()) * vg * MirrorMatrix(mirrored);
                const ViewInfo eqInfo = AnalyzeView(veq);
                CHECK(eqInfo.Valid && eqInfo.DetSign == 1);
                CHECK_NEAR(eqInfo.Scale, 1.0, 1e-12);
                CHECK_LE(eqInfo.OrthoError, 1e-12);
                const EyeFov eqFov = flipR ? Swapped(g.Tangents) : g.Tangents;
                Rows out{};
                CHECK(BuildEyeRows(f, veq, vg, mirrored, eqFov, &out));
                CHECK_LE(RowsError(out, f), kRowTolerance);
                Rows sky{};
                CHECK(BuildSkyRows(f, veq, vg, mirrored, eqFov, &sky));
                CHECK_LE(RowsError(sky, f), kRowTolerance);

                // The same eye through the rig and an OpenXR-style pose: E = R_rig^-1 * V_eq^-1 * Sc(s).
                Rig rig;
                rig.Anchor = ToVrWorld(kLinkKokiri, mirrored);
                rig.Yaw = 0.7;
                const Mat4 e = WorldToRig(rig) * Inverse(veq) * Scaling(s);
                const ViewInfo eInfo = AnalyzeView(e);
                CHECK(eInfo.DetSign == 1);
                CHECK_LE(eInfo.OrthoError, 1e-12);
                const Pose eyePose{QuatFromMatrix(e), {e.M[0][3], e.M[1][3], e.M[2][3]}};
                const Mat4 vi = EyeView(rig, eyePose);
                CHECK_LE(MaxAbsDifference(vi, veq), 1e-9);
                Rows viaRig{};
                CHECK(BuildEyeRows(f, vi, vg, mirrored, eqFov, &viaRig));
                CHECK_LE(RowsError(viaRig, f), kRowTolerance);

                // Winding: triangles seen by a perturbed eye keep the game's screen-space orientation, so
                // the game's own cull mode stays right (no front-face change).
                const Mat4 perturbed = Translation({0.03 * s, -0.02 * s, 0.05 * s}) * RotationY(DegreesToRadians(6.0)) *
                                       RotationX(DegreesToRadians(-4.0)) * veq;
                Rows eyeRows{};
                CHECK(BuildEyeRows(f, perturbed, vg, mirrored, SymmetricFov(DegreesToRadians(50.0)), &eyeRows));
                const Mat4 eyeToGameView = Inverse(EyeChain(perturbed, Inverse(vg), mirrored));
                Rng rng;
                for (int i = 0; i < 200; ++i) {
                    // A small triangle in front of the perturbed eye, 2-40 m away, inside its view.
                    const double depth = rng.Range(2.0, 40.0) * s;
                    const Vec3 centre{rng.Range(-0.6, 0.6) * depth, rng.Range(-0.4, 0.4) * depth, -depth};
                    Vec3 corners[3];
                    for (Vec3& corner : corners) {
                        const Vec3 eyePoint = centre + Vec3{rng.Range(-0.1, 0.1), rng.Range(-0.1, 0.1),
                                                            rng.Range(-0.05, 0.05)} * depth;
                        corner = TransformPoint(eyeToGameView, eyePoint);  // game view space (what f[] multiplies)
                    }
                    bool inFront = true;
                    for (const Vec3& corner : corners) inFront = inFront && Clip(f, corner)[3] > 0.0;
                    if (!inFront) continue;  // behind the game camera: the game never drew it
                    const double gameArea = SignedArea(Ndc(f, corners[0]), Ndc(f, corners[1]), Ndc(f, corners[2]));
                    const double eyeArea =
                        SignedArea(Ndc(eyeRows, corners[0]), Ndc(eyeRows, corners[1]), Ndc(eyeRows, corners[2]));
                    if (std::fabs(gameArea) < 1e-12) continue;
                    CHECK((gameArea > 0.0) == (eyeArea > 0.0));
                    ++trianglesTested;
                }

                // True stereo: the left eye sees a near point right of a far point straight ahead, the right
                // eye left of it, and up stays up, in landscape display coordinates (R+^T of the target).
                const double halfIpd = 0.032 * s;
                const Mat4 left = Translation({halfIpd, 0.0, 0.0}) * veq;
                const Mat4 right = Translation({-halfIpd, 0.0, 0.0}) * veq;
                const Mat4 headToGameView = Inverse(EyeChain(veq, Inverse(vg), mirrored));
                const Vec3 nearPoint = TransformPoint(headToGameView, {0.0, 0.0, -1.0 * s});
                const Vec3 farPoint = TransformPoint(headToGameView, {0.0, 0.0, -20.0 * s});
                const Vec3 upPoint = TransformPoint(headToGameView, {0.0, 0.5 * s, -5.0 * s});
                const EyeFov eyeFov = SymmetricFov(DegreesToRadians(50.0));
                Rows leftRows{}, rightRows{};
                CHECK(BuildEyeRows(f, left, vg, mirrored, eyeFov, &leftRows));
                CHECK(BuildEyeRows(f, right, vg, mirrored, eyeFov, &rightRows));
                const Rot2 rp = RotPlus(g.Rot3ds);
                const Screen leftNear = Display(rp, Ndc(leftRows, nearPoint));
                const Screen leftFar = Display(rp, Ndc(leftRows, farPoint));
                const Screen rightNear = Display(rp, Ndc(rightRows, nearPoint));
                const Screen rightFar = Display(rp, Ndc(rightRows, farPoint));
                CHECK(leftNear.X > leftFar.X);
                CHECK(rightNear.X < rightFar.X);
                CHECK(Display(rp, Ndc(leftRows, upPoint)).Y > 0.0);

                if (flipR && !flipV) {
                    // The rejected alternative (keep det R = -1, flip each eye image, M = I) inverts depth.
                    const Mat4 altLeft = Translation({halfIpd, 0.0, 0.0}) * vg;
                    const Mat4 x = EyeProjection(g, eyeFov) * altLeft * Inverse(vg);
                    Rows alt{};
                    for (int k = 0; k < 4; ++k) {
                        alt[0][k] = static_cast<float>(r[0][0] * x.M[0][k] + r[0][1] * x.M[1][k]);
                        alt[1][k] = static_cast<float>(r[1][0] * x.M[0][k] + r[1][1] * x.M[1][k]);
                        alt[2][k] = static_cast<float>(x.M[2][k]);
                        alt[3][k] = static_cast<float>(x.M[3][k]);
                    }
                    const Vec3 n{0.0, 0.0, -1.0 * s}, fp{0.0, 0.0, -20.0 * s};
                    CHECK(Display(vanilla, Ndc(alt, n)).X < Display(vanilla, Ndc(alt, fp)).X);
                }
            }
        }
    }
    CHECK(trianglesTested > 1000);
    Report("mirror parity");
    std::printf("               (%d winding triangles)\n", trianglesTested);
}

void TestRig() {
    Rig rig;
    rig.Anchor = {-1480.0, 12.0, 1120.0};
    rig.Yaw = DegreesToRadians(37.0);
    CHECK_LE(MaxAbsDifference(RigToWorld(rig) * WorldToRig(rig), Mat4::Identity()), 1e-12);
    Rng rng;
    for (int i = 0; i < 100; ++i) {
        const Vec3 p{rng.Range(-3, 3), rng.Range(0, 2.5), rng.Range(-3, 3)};
        const Vec3 back = WorldToTracking(rig, TrackingToWorld(rig, p));
        CHECK_LE(Length(back - p), 1e-12);
        const Vec3 q{rng.Range(-3, 3), rng.Range(0, 2.5), rng.Range(-3, 3)};
        CHECK_NEAR(Length(TrackingToWorld(rig, p) - TrackingToWorld(rig, q)), kDefaultScale * Length(p - q), 1e-9);
    }
    // T forward -Z maps to (sin psi, 0, cos psi); the STAGE origin maps to the anchor.
    const Vec3 forward = TrackingDirectionToWorld(rig, {0.0, 0.0, -1.0});
    CHECK_NEAR(forward.X, std::sin(rig.Yaw), 1e-12);
    CHECK_NEAR(forward.Z, std::cos(rig.Yaw), 1e-12);
    CHECK_LE(Length(TrackingToWorld(rig, {}) - rig.Anchor), 1e-12);
    CHECK_NEAR(TrackingYawToWorld(rig, YawOf({0.0, 0.0, -1.0})), rig.Yaw, 1e-12);

    // An OpenXR eye 1.7 m up, turned +90 deg about +Y (a left turn): gaze yaw psi + 90 deg.
    const Pose eye = HeadPose({0.3, 1.7, -0.2}, 90.0);
    const WorldHead head = HeadToWorld(rig, eye);
    CHECK_NEAR(WrapAngle(head.Yaw - rig.Yaw), DegreesToRadians(90.0), 1e-12);
    CHECK_NEAR(head.Pitch, 0.0, 1e-12);
    CHECK_NEAR(head.Position.Y, rig.Anchor.Y + 1.7 * kDefaultScale, 1e-9);
    const Mat4 vi = EyeView(rig, eye);
    const ViewInfo info = AnalyzeView(vi);
    CHECK(info.Valid && info.DetSign == 1);
    CHECK_NEAR(info.Scale, 1.0, 1e-12);
    CHECK_LE(Length(TransformPoint(vi, head.Position)), 1e-9);
    const Vec3 ahead = TransformPoint(vi, head.Position + head.Forward * kDefaultScale);
    CHECK_LE(Length(ahead - Vec3{0.0, 0.0, -kDefaultScale}), 1e-9);
    const Vec3 up = TransformDirection(vi, {0.0, 1.0, 0.0});
    CHECK_LE(Length(up - Vec3{0.0, 1.0, 0.0}), 1e-12);
    // The eye's right (+X) is the world direction 90 deg clockwise of its gaze (yaw - 90 deg).
    const Vec3 rightW = YawDirection(head.Yaw - kPi / 2.0);
    CHECK_LE(Length(TransformDirection(vi, rightW) - Vec3{1.0, 0.0, 0.0}), 1e-12);

    // Pitch up 20 deg.
    CHECK_NEAR(HeadToWorld(rig, HeadPose({0.0, 1.6, 0.0}, 0.0, 20.0)).Pitch, DegreesToRadians(20.0), 1e-12);

    // Floats in XrPosef order (x, y, z, w): yaw +90 deg looks along T -X.
    const float orientation[4] = {0.0F, 0.70710678F, 0.0F, 0.70710678F};
    const float position[3] = {0.1F, 1.6F, 0.2F};
    const Pose fromXr = PoseFromXr(orientation, position);
    CHECK_LE(Length(Rotate(fromXr.Orientation, {0.0, 0.0, -1.0}) - Vec3{-1.0, 0.0, 0.0}), 1e-7);
    CHECK_NEAR(fromXr.Position.Y, 1.6, 1e-6);

    // Quaternion <-> matrix through every branch of the conversion.
    for (const auto& [axis, angle] : std::vector<std::pair<Vec3, double>>{{{1, 0, 0}, kPi},
                                                                           {{0, 1, 0}, kPi},
                                                                           {{0, 0, 1}, kPi},
                                                                           {{1, 2, 3}, 0.4},
                                                                           {{-1, 0.5, 0.2}, 2.9}}) {
        const Mat4 m = RotationMatrix(QuatFromAxisAngle(axis, angle));
        CHECK_LE(MaxAbsDifference(RotationMatrix(QuatFromMatrix(m)), m), 1e-12);
    }

    // Game <-> VR world through the mirror.
    const Vec3 p{12.0, 3.0, -7.0};
    CHECK_LE(Length(ToGameWorld(ToVrWorld(p, true), true) - p), 0.0);
    CHECK_NEAR(ToVrWorld(p, true).X, -12.0, 0.0);
    CHECK_LE(Length(ToVrWorld(p, false) - p), 0.0);
    CHECK_NEAR(YawToVrWorld(0.5, true), -0.5, 0.0);
    CHECK_NEAR(YawToVrWorld(0.5, false), 0.5, 0.0);
    const WorldHead game = HeadToGameWorld(head, true);
    CHECK_NEAR(game.Position.X, -head.Position.X, 0.0);
    CHECK_NEAR(game.Yaw, WrapAngle(-head.Yaw), 1e-12);
    CHECK_NEAR(game.Yaw, YawOf(game.Forward), 1e-12);

    // Binary angles and wrapping.
    CHECK_NEAR(BinAngleToRadians(0x4000), kPi / 2.0, 1e-15);
    CHECK(RadiansToBinAngle(kPi) == static_cast<int16_t>(-32768));
    CHECK(RadiansToBinAngle(-kPi / 2.0) == static_cast<int16_t>(-0x4000));
    CHECK(RadiansToBinAngle(BinAngleToRadians(0x1234)) == 0x1234);
    CHECK_NEAR(WrapAngle(3.0 * kPi), kPi, 1e-12);
    CHECK_NEAR(WrapAngle(-kPi), kPi, 0.0);
    CHECK_NEAR(WrapAngle(-0.25), -0.25, 0.0);
    Report("rig");
}

void TestSky() {
    const Mat4 camera = KokiriCamera();
    const Rows f = RowsFromTangents(kPortraitA, GameFovCentred(), -1.0);
    const GameProjection g = DecomposeGameProjection(f);
    const EyeFov eyeFov = SymmetricFov(DegreesToRadians(50.0));

    // Rotation only: the eye position and the game camera's translation do not change the rows.
    const Mat4 eye = RotationY(DegreesToRadians(25.0)) * camera;
    Rows a{}, b{}, c{};
    CHECK(BuildSkyRows(f, eye, camera, false, eyeFov, &a));
    CHECK(BuildSkyRows(f, Translation({40.0, -12.0, 90.0}) * eye, camera, false, eyeFov, &b));
    CHECK(BuildSkyRows(f, eye, camera * Translation({300.0, 50.0, -800.0}), false, eyeFov, &c));
    CHECK_LE(RowsError(a, b), 1e-6);
    CHECK_LE(RowsError(a, c), 1e-6);
    // Zero translation response: the camera-space origin maps to x = y = 0 and the game's own z/w.
    CHECK(a[0][3] == 0.0F && a[1][3] == 0.0F);
    CHECK(a[2][3] == f[2][3] && a[3][3] == f[3][3]);
    // A sky model whose origin is the game camera still sits on the eye.
    const Vec4 origin = Clip(a, {});
    CHECK(origin[0] == 0.0 && origin[1] == 0.0);
    // At the game camera's orientation (any position) the sky rows are the game's.
    Rows same{};
    CHECK(BuildSkyRows(f, Translation({3.0, 4.0, 5.0}) * camera, camera, false, g.Tangents, &same));
    CHECK_LE(RowsError(same, f), kRowTolerance);
    // A uniform scale in V_g changes nothing (Rot removes it).
    Rows scaled{};
    CHECK(BuildSkyRows(f, eye, Scaling(2.0) * camera, false, eyeFov, &scaled));
    CHECK_LE(RowsError(scaled, a), 1e-6);
    // The sky direction the eye looks along lands at the centre of the eye.
    const Mat4 eyeToWorld = Inverse(eye);
    const Vec3 gazeWorld = TransformDirection(eyeToWorld, {0.0, 0.0, -1.0});
    const Vec3 gazeGameView = TransformDirection(camera, gazeWorld) * 5000.0;
    const Screen centre = Ndc(a, gazeGameView);
    CHECK_NEAR(centre.X, 0.0, 1e-6);
    CHECK_NEAR(centre.Y, 0.0, 1e-6);
    // The full transform does respond to translation (contrast).
    Rows full1{}, full2{};
    CHECK(BuildEyeRows(f, eye, camera, false, eyeFov, &full1));
    CHECK(BuildEyeRows(f, Translation({40.0, 0.0, 0.0}) * eye, camera, false, eyeFov, &full2));
    CHECK(RowsError(full1, full2) > 1e-3);
    Report("sky");
}

void TestPlacement() {
    PlacementConfig config;
    const double linkYaw = DegreesToRadians(-120.0);
    PlacementInput input;
    input.LinkWorld = {kLinkKokiri.X, 4.0, kLinkKokiri.Z};
    input.LinkFacingYaw = linkYaw;

    const auto checkPlaced = [&](const PlacementResult& r, const PlacementInput& in) {
        CHECK(r.Valid);
        // Link lands where solved, on his floor, facing away from Jamal.
        CHECK_LE(Length(TrackingToWorld(r.NewRig, r.LinkT) - in.LinkWorld), 1e-9);
        CHECK_NEAR(r.NewRig.Anchor.Y, in.LinkWorld.Y, 1e-12);
        CHECK_NEAR(WrapAngle(TrackingYawToWorld(r.NewRig, YawOf(r.FacingT)) - in.LinkFacingYaw), 0.0, 1e-12);
        const Vec3 headW = TrackingToWorld(r.NewRig, in.Head.Position);
        const Vec3 facingW = YawDirection(in.LinkFacingYaw);
        CHECK(Dot(headW - in.LinkWorld, facingW) < 0.0);  // Jamal is behind Link
        CHECK_NEAR(DistanceXZ(r.LinkT, in.Head.Position), r.DistanceM, 1e-12);
    };

    {  // Jamal 1.5 m behind the centre: Link at c, 1.5 m ahead.
        input.Head = HeadPose({0.0, 1.7, 1.5}, 0.0);
        const PlacementResult r = SolvePlacement(input, config);
        checkPlaced(r, input);
        CHECK_LE(LengthXZ(r.LinkT), 1e-12);
        CHECK_NEAR(r.DistanceM, 1.5, 1e-12);
        CHECK(!r.NearCentre);
        CHECK_NEAR(r.NewRig.Yaw, linkYaw, 1e-12);  // facing T -Z: psi is Link's yaw
    }
    {  // 2.5 m away: d0 capped at 2 m, Link 2 m ahead along the line to c.
        input.Head = HeadPose({2.5, 1.7, 0.0}, 45.0);
        const PlacementResult r = SolvePlacement(input, config);
        checkPlaced(r, input);
        CHECK_NEAR(r.DistanceM, 2.0, 1e-12);
        CHECK_NEAR(r.LinkT.X, 0.5, 1e-12);
        CHECK_NEAR(r.FacingT.X, -1.0, 1e-12);
    }
    {  // within 0.6 m of c: Link 1.0 m ahead of the horizontal gaze (yaw -90 deg looks along T +X).
        input.Head = HeadPose({0.2, 1.7, 0.1}, -90.0, -30.0);
        const PlacementResult r = SolvePlacement(input, config);
        checkPlaced(r, input);
        CHECK(r.NearCentre);
        CHECK_NEAR(r.LinkT.X, 1.2, 1e-12);
        CHECK_NEAR(r.LinkT.Z, 0.1, 1e-12);
    }
    {  // probes shorten d0, never below 0.7 m
        input.Head = HeadPose({0.0, 1.7, 1.5}, 0.0);
        input.ProbeDistanceM = 0.9;
        PlacementResult r = SolvePlacement(input, config);
        checkPlaced(r, input);
        CHECK_NEAR(r.LinkT.Z, 0.6, 1e-12);
        input.ProbeDistanceM = 0.3;
        r = SolvePlacement(input, config);
        CHECK_NEAR(r.DistanceM, 0.7, 1e-12);
        input.ProbeDistanceM = std::numeric_limits<double>::infinity();
    }
    {  // standing on c, looking straight down after a 30 deg turn: the head's up vector gives the heading
        input.Head = HeadPose({0.0, 1.7, 0.0}, 30.0, -90.0);
        const PlacementResult r = SolvePlacement(input, config);
        checkPlaced(r, input);
        const Vec3 expected = YawDirection(YawOf(Rotate(HeadPose({}, 30.0).Orientation, {0.0, 0.0, -1.0})));
        CHECK_LE(Length(r.FacingT - expected), 1e-9);
        input.Head = HeadPose({0.0, 1.7, 0.0}, 30.0, 90.0);  // straight up
        CHECK_LE(Length(SolvePlacement(input, config).FacingT - expected), 1e-9);
    }
    {  // no bounds: c is the head at the first placement (here away from the T origin)
        input.CentreT = {1.0, 0.0, -2.0};
        input.Head = HeadPose({1.0, 1.7, -0.5}, 0.0);
        const PlacementResult r = SolvePlacement(input, config);
        checkPlaced(r, input);
        CHECK_LE(DistanceXZ(r.LinkT, input.CentreT), 1e-12);
        input.CentreT = {};
    }
    {  // mirrored frame: Link goes in through M, comes back out through M
        const Vec3 linkGame{-950.0, 120.0, 430.0};
        const double linkGameYaw = 0.9;
        PlacementInput mirroredInput = input;
        mirroredInput.LinkWorld = ToVrWorld(linkGame, true);
        mirroredInput.LinkFacingYaw = YawToVrWorld(linkGameYaw, true);
        mirroredInput.Head = HeadPose({0.4, 1.7, 1.2}, 10.0);
        const PlacementResult r = SolvePlacement(mirroredInput, config);
        checkPlaced(r, mirroredInput);
        CHECK_LE(Length(ToGameWorld(TrackingToWorld(r.NewRig, r.LinkT), true) - linkGame), 1e-9);
    }
    {  // invalid input
        PlacementInput bad = input;
        bad.LinkWorld.X = std::numeric_limits<double>::quiet_NaN();
        CHECK(!SolvePlacement(bad, config).Valid);
        bad = input;
        bad.Head.Position.Z = std::numeric_limits<double>::infinity();
        CHECK(!SolvePlacement(bad, config).Valid);
    }
    Report("placement");
}

// Places Link at c with Jamal 1 m behind, standing, for the tether tests.
Rig PlacedRig(const Vec3& linkWorld, double linkYaw, const Vec3& headT) {
    PlacementInput input;
    input.LinkWorld = linkWorld;
    input.LinkFacingYaw = linkYaw;
    input.Head = HeadPose(headT, 0.0);
    return SolvePlacement(input, PlacementConfig{}).NewRig;
}

void TestTether() {
    const double s = kDefaultScale;
    const double dt = 1.0 / 30.0;
    {  // Leash: Link runs straight away at 3.6 m/s; a blink exactly when he passes 3.5 m, landing 1.0 m ahead.
        const Vec3 headT{0.0, 1.7, 1.0};
        const double linkYaw = DegreesToRadians(70.0);
        Rig rig = PlacedRig(kLinkKokiri, linkYaw, headT);
        const double placedYaw = rig.Yaw, placedY = rig.Anchor.Y;
        TetherConfig config;
        TetherState state;
        Vec3 link = kLinkKokiri;
        const Vec3 run = YawDirection(linkYaw) * (3.6 * s * dt);
        int blinks = 0;
        int firstBlink = -1;
        for (int frame = 0; frame < 300; ++frame) {
            link = link + run;
            const double before = DistanceXZ(WorldToTracking(rig, link), headT);
            const TetherStep step = UpdateTether(state, config, rig, link, linkYaw, headT, dt);
            CHECK((step.Event == RigEvent::Blink) == (before > config.LeashM));
            CHECK(step.Event == RigEvent::None || step.Event == RigEvent::Blink);
            if (step.Event == RigEvent::Blink) {
                ++blinks;
                if (firstBlink < 0) firstBlink = frame;
                const Vec3 linkT = WorldToTracking(step.NewRig, link);
                CHECK_NEAR(DistanceXZ(linkT, headT), 1.0, 1e-9);
                CHECK_LE(Length(linkT - step.LinkT), 1e-9);
                // Along Link's travel direction (in T), psi and a.y unchanged.
                const Vec3 travelT = NormalizedXZ(WorldDirectionToTracking(rig, run), {});
                CHECK_NEAR(Dot(NormalizedXZ(linkT - headT, {}), travelT), 1.0, 1e-9);
                CHECK_NEAR(step.NewRig.Yaw, placedYaw, 0.0);
                CHECK_NEAR(step.NewRig.Anchor.Y, placedY, 0.0);
            }
            rig = step.NewRig;
        }
        // From 1.0 m to 3.5 m at 3.6 m/s: a blink every ~0.69 s (first one from 1.0 m behind c too).
        CHECK(firstBlink >= 20 && firstBlink <= 22);
        CHECK(blinks >= 13 && blinks <= 15);
    }
    {  // No blink inside the leash: Link and Jamal wander, Link always moving, within 3.4 m (unbounded room).
        Vec3 headT{0.0, 1.7, 0.8};
        Rig rig = PlacedRig(kLinkKokiri, 0.3, headT);
        const Rig placed = rig;
        TetherConfig config;
        TetherState state;
        Rng rng;
        Vec3 link = kLinkKokiri;
        double heading = 0.0;
        int events = 0;
        for (int frame = 0; frame < 30 * 60; ++frame) {
            heading += rng.Range(-0.3, 0.3);
            Vec3 next = link + YawDirection(heading) * (1.2 * s * dt);
            if (DistanceXZ(WorldToTracking(rig, next), headT) > 3.4) {
                heading += kPi;
                next = link + YawDirection(heading) * (1.2 * s * dt);
            }
            link = next;
            headT = headT + Vec3{rng.Range(-0.01, 0.01), 0.0, rng.Range(-0.01, 0.01)};
            const TetherStep step = UpdateTether(state, config, rig, link, heading, headT, dt);
            if (step.Event != RigEvent::None) ++events;
            rig = step.NewRig;
        }
        CHECK(events == 0);
        CHECK_LE(Length(rig.Anchor - placed.Anchor), 0.0);
        CHECK(rig.Yaw == placed.Yaw);
    }
    {  // Play area: Link more than 2 m outside the bounds (inside the leash) blinks; Jamal's own excursion counts.
        const Vec3 headT{0.9, 1.7, 0.0};
        const Rig rig = PlacedRig(kLinkKokiri, 0.0, {0.0, 1.7, 1.0});
        TetherConfig config;
        config.Area = {1.0, 1.0, true};
        const auto linkAtT = [&](double x) { return TrackingToWorld(rig, {x, 0.0, 0.0}); };
        TetherState state;
        CHECK(UpdateTether(state, config, rig, linkAtT(2.9), 0.0, headT, dt).Event == RigEvent::None);
        TetherState state2;
        const TetherStep out = UpdateTether(state2, config, rig, linkAtT(3.1), 0.0, headT, dt);
        CHECK(out.Event == RigEvent::Blink);
        CHECK_NEAR(out.HeadDistanceM, 1.0, 1e-9);
        TetherState state3;
        CHECK(UpdateTether(state3, config, rig, linkAtT(3.4), 0.0, {1.5, 1.7, 0.0}, dt).Event == RigEvent::None);
        CHECK_NEAR(RoomRadius(config.Area), 0.7, 1e-12);
        CHECK_NEAR(RoomRadius({0.5, 3.0, true}), 0.6, 1e-12);
        CHECK_NEAR(RoomRadius({}), 1.0, 1e-12);
    }
    {  // Idle re-centre: Link still 0.75 s, beyond 0.7 r_room from c, Jamal off c -> Link back to c.
        const Vec3 headT{-0.8, 1.7, 0.5};
        const Rig rig = PlacedRig(kLinkKokiri, 0.4, {0.0, 1.7, 1.0});
        TetherConfig config;
        config.Area = {1.5, 1.5, true};  // r_room 1.2, threshold 0.84
        const Vec3 link = TrackingToWorld(rig, {1.0, 0.0, 0.0});
        TetherState state;
        Rig current = rig;
        int idleFrame = -1;
        for (int frame = 0; frame < 60; ++frame) {
            const TetherStep step = UpdateTether(state, config, current, link, 0.4, headT, dt);
            if (step.Event == RigEvent::Idle) {
                CHECK(idleFrame < 0);
                idleFrame = frame;
                CHECK_LE(LengthXZ(WorldToTracking(step.NewRig, link)), 1e-9);
                CHECK(step.NewRig.Yaw == rig.Yaw && step.NewRig.Anchor.Y == rig.Anchor.Y);
            } else {
                CHECK(step.Event == RigEvent::None);
            }
            current = step.NewRig;
        }
        // The first update only seeds the motion history: still from the 2nd, 0.75 s at the 24th.
        CHECK(idleFrame == 23);
        TetherState onCentre;  // Jamal standing on c: never re-centred onto him
        bool fired = false;
        for (int frame = 0; frame < 90; ++frame)
            fired |= UpdateTether(onCentre, config, rig, link, 0.4, {0.2, 1.7, 0.1}, dt).Event != RigEvent::None;
        CHECK(!fired);
    }
    {  // Glide: the anchor takes exactly the excess beyond r_room; Link stays on the r_room circle.
        const Vec3 headT{0.0, 1.7, 1.0};
        Rig rig = PlacedRig(kLinkKokiri, 0.0, headT);
        const double yaw = rig.Yaw;
        TetherConfig config;
        config.Mode = TetherMode::Glide;  // unbounded: r_room = 1.0 m
        TetherState state;
        const Vec3 direction = TrackingDirectionToWorld(rig, {1.0, 0.0, 0.0});
        Vec3 link = kLinkKokiri;
        int glides = 0;
        for (int frame = 0; frame < 120; ++frame) {
            link = link + direction * (1.0 * s * dt);
            const TetherStep step = UpdateTether(state, config, rig, link, 0.0, headT, dt);
            const double fromCentre = LengthXZ(WorldToTracking(step.NewRig, link));
            CHECK_LE(fromCentre, 1.0 + 1e-9);
            if (step.Event == RigEvent::Glide) {
                ++glides;
                CHECK_NEAR(fromCentre, 1.0, 1e-9);
                if (glides > 2) CHECK_NEAR(step.AnchorSpeedMps, 1.0, 1e-6);
            }
            CHECK(step.NewRig.Yaw == yaw);
            rig = step.NewRig;
        }
        CHECK(glides > 80);
    }
    {  // A teleport step gives no travel direction: the blink uses the last real travel.
        const Vec3 headT{0.0, 1.7, 1.0};
        const Rig rig = PlacedRig(kLinkKokiri, 0.0, headT);
        TetherConfig config;
        TetherState state;
        Vec3 link = kLinkKokiri;
        const Vec3 walk = TrackingDirectionToWorld(rig, {0.0, 0.0, -1.0}) * (1.0 * s * dt);
        for (int frame = 0; frame < 5; ++frame) {
            link = link + walk;
            CHECK(UpdateTether(state, config, rig, link, 0.0, headT, dt).Event == RigEvent::None);
        }
        link = link + TrackingDirectionToWorld(rig, {1.0, 0.0, 0.0}) * (50.0 * s);
        const TetherStep step = UpdateTether(state, config, rig, link, kPi / 2.0, headT, dt);
        CHECK(step.Event == RigEvent::Blink);
        const Vec3 landed = NormalizedXZ(WorldToTracking(step.NewRig, link) - headT, {});
        CHECK_LE(Length(landed - Vec3{0.0, 0.0, -1.0}), 1e-9);
    }
    Report("tether");
}

void TestSnapTurn() {
    SnapTurnDetector detector;
    CHECK(detector.Update(0.5) == 0);
    CHECK(detector.Update(0.8) == 1);
    CHECK(detector.Update(0.9) == 0);   // held
    CHECK(detector.Update(0.5) == 0);   // not re-armed above 0.3
    CHECK(detector.Update(0.8) == 0);
    CHECK(detector.Update(0.2) == 0);   // re-armed
    CHECK(detector.Update(-0.75) == -1);
    CHECK(detector.Update(-0.1) == 0);
    CHECK(detector.Update(0.71) == 1);

    Rig rig;
    rig.Anchor = {-1480.0, 10.0, 1120.0};
    rig.Yaw = DegreesToRadians(170.0);
    const Pose head = HeadPose({0.4, 1.7, -0.3}, 20.0);
    const WorldHead before = HeadToWorld(rig, head);
    const Rig right = SnapTurn(rig, head.Position, 1);
    const WorldHead afterRight = HeadToWorld(right, head);
    CHECK_LE(Length(afterRight.Position - before.Position), 1e-9);  // pivot on the head
    CHECK_NEAR(WrapAngle(afterRight.Yaw - before.Yaw), DegreesToRadians(-45.0), 1e-12);  // right lowers the yaw
    CHECK_NEAR(right.Anchor.Y, rig.Anchor.Y, 0.0);
    const WorldHead afterLeft = HeadToWorld(SnapTurn(rig, head.Position, -1, DegreesToRadians(30.0)), head);
    CHECK_LE(Length(afterLeft.Position - before.Position), 1e-9);
    CHECK_NEAR(WrapAngle(afterLeft.Yaw - before.Yaw), DegreesToRadians(30.0), 1e-12);
    CHECK(SnapTurn(rig, head.Position, 0).Yaw == rig.Yaw);
    // Eight right snaps are a full turn: the rig comes back.
    Rig spun = rig;
    for (int i = 0; i < 8; ++i) spun = SnapTurn(spun, head.Position, 1);
    CHECK_NEAR(WrapAngle(spun.Yaw - rig.Yaw), 0.0, 1e-12);
    CHECK_LE(Length(spun.Anchor - rig.Anchor), 1e-9);
    CHECK(spun.Yaw > -kPi && spun.Yaw <= kPi);
    // Vignette strength (§5.7).
    CHECK_NEAR(VignetteStrength(1.5, 0.0), 0.5, 1e-12);
    CHECK_NEAR(VignetteStrength(0.0, kPi / 4.0), 0.5, 1e-12);
    CHECK_NEAR(VignetteStrength(-9.0, 0.0), 1.0, 0.0);
    Report("snap turn");
}

void TestFloor() {
    const double s = kDefaultScale;
    const double dt = 1.0 / 30.0;
    FloorConfig config;
    const auto ground = [&](double floorY, double linkY) {
        FloorInput in;
        in.FloorY = floorY;
        in.LinkY = linkY;
        in.Dt = dt;
        in.EyeHeightM = 1.7;
        return in;
    };
    {
        FloorState state;
        ResetFloor(state, 0.0);
        // Small steps inside the hysteresis do nothing.
        for (int i = 0; i < 30; ++i) CHECK(UpdateFloor(state, config, ground(0.1 * s, 0.1 * s)).AnchorY == 0.0);
        // A 0.5 m ledge: glide, monotonic, no overshoot, settled within 1.5 s.
        double last = 0.0;
        bool sawSpeed = false;
        int settled = -1;
        for (int i = 0; i < 60; ++i) {
            const FloorStep step = UpdateFloor(state, config, ground(0.5 * s, 0.5 * s));
            CHECK(!step.Blink);
            CHECK(step.AnchorY >= last && step.AnchorY <= 0.5 * s);
            sawSpeed |= step.SpeedMps > 0.0;
            last = step.AnchorY;
            if (settled < 0 && !step.Gliding) settled = i;
        }
        CHECK(sawSpeed);
        CHECK(settled > 0 && settled < 45);
        CHECK_NEAR(state.AnchorY, 0.5 * s, 0.0);
        // A jump (airborne: Link well above his floor) never bobs the world.
        for (int i = 0; i < 20; ++i) {
            const double height = 30.0 * std::sin(kPi * i / 19.0) + 2.5;
            CHECK(UpdateFloor(state, config, ground(0.5 * s, 0.5 * s + height)).AnchorY == 0.5 * s);
        }
        // Hanging then climbing a 2 m ledge: hold; standing on top: a blink (> 1.5 m).
        FloorInput hang = ground(2.5 * s, 2.0 * s);
        hang.Hanging = true;
        CHECK(UpdateFloor(state, config, hang).AnchorY == 0.5 * s);
        hang.Hanging = false;
        hang.Climbing = true;
        hang.LinkY = 2.5 * s;
        CHECK(UpdateFloor(state, config, hang).AnchorY == 0.5 * s);
        const FloorStep top = UpdateFloor(state, config, ground(2.5 * s, 2.5 * s));
        CHECK(top.Blink && top.AnchorY == 2.5 * s);
        // Falling 3 m: a blink on landing; no floor (NaN): hold.
        const FloorStep fall = UpdateFloor(state, config, ground(-0.5 * s, -0.5 * s));
        CHECK(fall.Blink && fall.AnchorY == -0.5 * s);
        CHECK(UpdateFloor(state, config, ground(std::numeric_limits<double>::quiet_NaN(), 0.0)).AnchorY == -0.5 * s);
    }
    {  // Swimming with a valid water height: the head 0.45 m above the surface, Link's bobbing ignored.
        FloorState state;
        ResetFloor(state, 30.0);
        const double water = 100.0;
        const double target = water + s * (0.45 - 1.7);
        FloorInput in = ground(std::numeric_limits<double>::quiet_NaN(), 95.0);
        in.Swimming = true;
        in.WaterY = water;
        for (int i = 0; i < 60; ++i) UpdateFloor(state, config, in);
        CHECK(state.Mode == FloorMode::Swim);
        CHECK_NEAR(state.AnchorY, target, 0.0);
        CHECK_NEAR(state.AnchorY + s * in.EyeHeightM, water + 0.45 * s, 1e-9);
        for (int i = 0; i < 60; ++i) {
            in.LinkY = 95.0 + 3.0 * std::sin(i * 0.4);
            CHECK(UpdateFloor(state, config, in).AnchorY == target);
        }
        // Diving: Link more than 1 m under for 0.5 s -> follow him down, continuously, never a blink.
        in.LinkY = 50.0;
        int diveFrame = -1;
        double previous = state.AnchorY;
        for (int i = 0; i < 90; ++i) {
            const FloorStep step = UpdateFloor(state, config, in);
            CHECK(!step.Blink);
            if (diveFrame < 0 && step.Mode == FloorMode::Dive) diveFrame = i;
            CHECK(step.AnchorY <= previous + 1e-12);
            previous = step.AnchorY;
        }
        CHECK(diveFrame >= 14 && diveFrame <= 15);
        const double diveTarget = 50.0 + s * (0.45 - 1.7);
        CHECK_NEAR(state.AnchorY, diveTarget, 0.05 * s);
        // The surface reading stays valid 1.5 m under (it repeats the accepted surface); garbage does not.
        CHECK_NEAR(state.WaterY, water, 0.0);
        in.WaterY = 300.0;
        CHECK(UpdateFloor(state, config, in).Mode == FloorMode::Dive);
        CHECK_NEAR(state.WaterY, water, 0.0);
        in.WaterY = water;
        // Back at the surface: the swim rule again.
        in.LinkY = 98.0;
        for (int i = 0; i < 90; ++i) UpdateFloor(state, config, in);
        CHECK(state.Mode == FloorMode::Swim);
        CHECK_NEAR(state.AnchorY, target, 0.0);
        // Climbing out onto the shore (1.40 m up from the swim anchor): glide, no blink.
        FloorInput shore = ground(105.0, 105.0);
        bool blinked = false;
        for (int i = 0; i < 90; ++i) blinked |= UpdateFloor(state, config, shore).Blink;
        CHECK(!blinked);
        CHECK(state.Mode == FloorMode::Ground);
        CHECK_NEAR(state.AnchorY, 105.0, 0.0);
    }
    {  // Water height missing or implausible: Link's low-passed y stands in.
        FloorState state;
        ResetFloor(state, 40.0);
        FloorInput in = ground(std::numeric_limits<double>::quiet_NaN(), 95.0);
        in.Swimming = true;
        for (int i = 0; i < 90; ++i) UpdateFloor(state, config, in);
        CHECK_NEAR(state.AnchorY, 95.0 + s * (0.45 - 1.7), 0.0);
        in.WaterY = 95.0 + 2.0 * s;  // more than 1.5 m from Link: ignored
        for (int i = 0; i < 30; ++i) UpdateFloor(state, config, in);
        CHECK_NEAR(state.AnchorY, 95.0 + s * (0.45 - 1.7), 0.0);
    }
    Report("floor");
}

void TestSeated() {
    const double s = kDefaultScale;
    CHECK_NEAR(SeatedYOffset(1.15), 0.45, 1e-12);
    CHECK_NEAR(SeatedYOffset(1.80), -0.20, 1e-12);
    CHECK_NEAR(SeatedYOffset(std::numeric_limits<double>::quiet_NaN()), 0.0, 0.0);
    CHECK_NEAR(SeatedYOffset(-5.0), kStandingEyeHeightM, 0.0);
    CHECK_NEAR(DefaultLeashM(true), 2.5, 0.0);
    CHECK_NEAR(DefaultLeashM(false), 3.5, 0.0);

    PlacementConfig config;
    config.SeatY = SeatedYOffset(1.15);
    PlacementInput input;
    input.LinkWorld = {kLinkKokiri.X, 18.0, kLinkKokiri.Z};
    input.LinkFacingYaw = 1.1;
    input.Head = HeadPose({0.05, 1.15, 0.1}, 15.0);  // seated near c: Link 1 m ahead of the gaze
    const PlacementResult r = SolvePlacement(input, config);
    CHECK(r.Valid && r.NearCentre);
    CHECK_NEAR(r.LinkT.Y, -0.45, 1e-12);
    CHECK_LE(Length(TrackingToWorld(r.NewRig, r.LinkT) - input.LinkWorld), 1e-9);
    // The seated eye sits at the standing eye height above Link's floor.
    CHECK_NEAR(TrackingToWorld(r.NewRig, input.Head.Position).Y, 18.0 + kStandingEyeHeightM * s, 1e-9);

    // The seated leash blinks at 2.5 m.
    TetherConfig tether;
    tether.LeashM = DefaultLeashM(true);
    TetherState state;
    const Vec3 headT = input.Head.Position;
    const Vec3 inside = TrackingToWorld(r.NewRig, headT + Vec3{0.0, 0.0, -2.45});
    const Vec3 outside = TrackingToWorld(r.NewRig, headT + Vec3{0.0, 0.0, -2.55});
    CHECK(UpdateTether(state, tether, r.NewRig, inside, 0.0, headT, 1.0 / 30.0).Event == RigEvent::None);
    CHECK(UpdateTether(state, tether, r.NewRig, outside, 0.0, headT, 1.0 / 30.0).Event == RigEvent::Blink);
    Report("seated");
}

void TestRobustness() {
    const EyeFov fov = GameFovCentred();
    const Rows good = RowsFromTangents(kPortraitA, fov, -1.0);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    {
        Rows f = good;
        f[1][2] = nan;
        CHECK(DecomposeGameProjection(f).Status == ProjectionStatus::NonFinite);
        f = good;
        f[2][3] = inf;
        CHECK(DecomposeGameProjection(f).Status == ProjectionStatus::NonFinite);
        f = good;
        f[3] = {0.0F, 0.0F, 0.0F, 0.0F};
        CHECK(DecomposeGameProjection(f).Status == ProjectionStatus::NotPerspective);
        f = good;
        f[3] = {0.0F, 0.0F, 0.0F, 1.0F};  // orthographic
        CHECK(DecomposeGameProjection(f).Status == ProjectionStatus::NotPerspective);
        f = good;
        f[3] = {0.0F, 0.0F, 1.0F, 0.0F};
        CHECK(DecomposeGameProjection(f).Status == ProjectionStatus::NonNegativeC);
        f = good;
        f[3][0] = 0.01F;
        CHECK(DecomposeGameProjection(f).Status == ProjectionStatus::NotPerspective);
        f = good;
        f[3][0] = 0.0005F;  // below the tolerance: still perspective
        CHECK(DecomposeGameProjection(f).Ok());
        f = good;
        f[0] = {0.0F, 0.0F, 0.0F, 0.0F};
        CHECK(DecomposeGameProjection(f).Status == ProjectionStatus::Degenerate);
        f = good;
        f[0][0] = 0.002F * f[0][1];  // off-axis 2e-3 of the scale
        CHECK(DecomposeGameProjection(f).Status == ProjectionStatus::OffAxis);
        f = good;
        f[0][0] = 0.0005F * f[0][1];  // 5e-4: tolerated
        CHECK(DecomposeGameProjection(f).Ok());
        f = good;
        f[1][3] = 0.01F;  // a clip-space translation (stereo shift) is not a camera
        CHECK(DecomposeGameProjection(f).Status == ProjectionStatus::OffAxis);
        f = good;
        f[0] = {1.0F, 1.0F, 0.0F, 0.0F};  // 45 deg: no signed permutation fits
        f[1] = {-1.0F, 1.0F, 0.0F, 0.0F};
        CHECK(DecomposeGameProjection(f).Status == ProjectionStatus::OffAxis);
        CHECK(std::string_view(ProjectionStatusName(ProjectionStatus::OffAxis)) == "off_axis");
    }
    {  // Eye rows refuse bad inputs and leave the output untouched.
        const GameProjection g = DecomposeGameProjection(good);
        const Mat4 camera = KokiriCamera();
        Rows out = good;
        out[0][0] = 42.0F;
        CHECK(!ComposeEyeRows(g, {0.5, 0.5, 0.5, -0.5}, Mat4::Identity(), &out));
        CHECK(!ComposeEyeRows(g, {-0.5, 0.5, std::nan(""), -0.5}, Mat4::Identity(), &out));
        Mat4 badChain = Mat4::Identity();
        badChain.M[0][3] = std::numeric_limits<double>::infinity();
        CHECK(!ComposeEyeRows(g, fov, badChain, &out));
        GameProjection notOk = g;
        notOk.Status = ProjectionStatus::NotPerspective;
        CHECK(!ComposeEyeRows(notOk, fov, Mat4::Identity(), &out));
        CHECK(out[0][0] == 42.0F);
        // A singular game view: no rows.
        Mat4 singular = camera;
        singular.M[2] = {0.0, 0.0, 0.0, 5.0};
        CHECK(!BuildEyeRows(good, camera, singular, false, fov, &out));
        CHECK(!BuildSkyRows(good, singular, camera, false, fov, &out));
        CHECK(out[0][0] == 42.0F);
    }
    {  // Affine inverse: singular, near-singular, non-affine, scaled and mirrored.
        Mat4 out = Mat4::Identity();
        CHECK(!InverseAffine(Mat4{}, &out));
        Mat4 rank2 = Mat4::Identity();
        rank2.M[2] = {1.0, 1.0, 0.0, 0.0};
        rank2.M[1] = {1.0, 1.0, 0.0, 0.0};
        CHECK(!InverseAffine(rank2, &out));
        Mat4 thin = KokiriCamera();
        for (int c = 0; c < 4; ++c) thin.M[1][c] *= 1e-13;
        CHECK(!InverseAffine(thin, &out));
        Mat4 projective = Mat4::Identity();
        projective.M[3] = {0.0, 0.0, -1.0, 0.0};
        CHECK(!InverseAffine(projective, &out));
        for (double k : {1e-3, 100.0}) {
            const Mat4 scaled = Scaling(k) * KokiriCamera();
            CHECK(InverseAffine(scaled, &out));
            CHECK_LE(MaxAbsDifference(scaled * out, Mat4::Identity()), 1e-9);
            // The identity property holds for a scaled game view too (V_i = V_g).
            Rows rows{};
            CHECK(BuildEyeRows(good, scaled, scaled, false, DecomposeGameProjection(good).Tangents, &rows));
            CHECK_LE(RowsError(rows, good), kRowTolerance);
            CHECK_NEAR(AnalyzeView(scaled).Scale, k, 1e-9 * k);
        }
        const Mat4 mirrored = KokiriCamera() * MirrorMatrix(true);
        CHECK(InverseAffine(mirrored, &out));
        CHECK_LE(MaxAbsDifference(out * mirrored, Mat4::Identity()), 1e-9);
        CHECK(!AnalyzeView(Mat4{}).Valid);
        CHECK(!MirroredFrame(-1, 0.0));
        CHECK(!MirroredFrame(1, std::nan("")));
        CHECK(MirroredFrame(-1, 1.0) && MirroredFrame(1, -1.0) && !MirroredFrame(-1, -1.0) && !MirroredFrame(1, 1.0));
    }
    {  // Quaternions and poses from bad data.
        bool valid = true;
        const Quat zero = Normalize({0.0, 0.0, 0.0, 0.0}, &valid);
        CHECK(!valid && zero.W == 1.0);
        const Quat bad = Normalize({std::nan(""), 0.0, 0.0, 1.0}, &valid);
        CHECK(!valid && bad.W == 1.0);
        const Quat unnormalised = Normalize({0.0, 2.0, 0.0, 2.0}, &valid);
        CHECK(valid);
        CHECK_NEAR(unnormalised.Y, std::sqrt(0.5), 1e-12);
        const float orientation[4] = {0.0F, 0.0F, 0.0F, 0.0F};
        const float position[3] = {nan, 1.0F, 2.0F};
        const Pose pose = PoseFromXr(orientation, position);
        CHECK(pose.Orientation.W == 1.0 && pose.Position.X == 0.0 && pose.Position.Y == 0.0);
        CHECK_LE(MaxAbsDifference(RotationMatrix({0.0, 0.0, 0.0, 3.0}), Mat4::Identity()), 1e-15);
        CHECK(QuatFromAxisAngle({0.0, 0.0, 0.0}, 1.0).W == 1.0);
    }
    {  // Rig state functions on degenerate input.
        Rig rig;
        TetherState state;
        const Vec3 nanLink{std::nan(""), 0.0, 0.0};
        const TetherStep step = UpdateTether(state, TetherConfig{}, rig, nanLink, 0.0, {0.0, 1.7, 0.0}, 1.0 / 30.0);
        CHECK(step.Event == RigEvent::None && IsFinite(step.NewRig.Anchor));
        const TetherStep zeroDt = UpdateTether(state, TetherConfig{}, rig, {1.0, 0.0, 1.0}, 0.0, {0.0, 1.7, 0.0}, 0.0);
        CHECK(IsFinite(zeroDt.NewRig.Anchor) && std::isfinite(zeroDt.AnchorSpeedMps));
        const TetherStep nanDt =
            UpdateTether(state, TetherConfig{}, rig, {1.0, 0.0, 1.0}, 0.0, {0.0, 1.7, 0.0}, std::nan(""));
        CHECK(IsFinite(nanDt.NewRig.Anchor));
        FloorState floor;
        ResetFloor(floor, 7.0);
        FloorInput in;
        in.LinkY = std::nan("");
        in.FloorY = 0.0;
        CHECK(UpdateFloor(floor, FloorConfig{}, in).AnchorY == 7.0);
        in.LinkY = 0.0;
        in.Dt = std::nan("");
        in.Scale = -1.0;
        CHECK(std::isfinite(UpdateFloor(floor, FloorConfig{}, in).AnchorY));
        FloorState fresh;  // uninitialised: starts on the floor, no jump
        FloorInput first;
        first.FloorY = 12.0;
        first.LinkY = 12.0;
        first.Dt = 1.0 / 30.0;
        CHECK(UpdateFloor(fresh, FloorConfig{}, first).AnchorY == 12.0);
        SnapTurnDetector detector;
        CHECK(detector.Update(std::nan("")) == 0);
        CHECK(WrapAngle(std::nan("")) == 0.0);
        CHECK(VignetteStrength(std::nan(""), std::nan("")) == 0.0);
        CHECK(RadiansToBinAngle(1e9) == RadiansToBinAngle(WrapAngle(1e9)));
        CHECK(std::string_view(RigEventName(RigEvent::Idle)) == "idle");
    }
    {  // Far from the origin (Hyrule Field extents) the identity still holds, through a pose from floats.
        const Mat4 camera = LookAt({-7900.0, 1800.0, 9100.0}, {-7850.0, 1700.0, 8850.0});
        const Rows f = RowsFromTangents(kPortraitB, GameFovOffCentre(), -1.0);
        Rows out{};
        CHECK(BuildEyeRows(f, camera, camera, false, DecomposeGameProjection(f).Tangents, &out));
        CHECK_LE(RowsError(out, f), kRowTolerance);
        // A float eye pose (as OpenXR delivers it) only perturbs the rows at the 1e-4 level here.
        Rig rig;
        rig.Anchor = {-7860.0, 1650.0, 8900.0};
        rig.Yaw = 2.2;
        const Mat4 e = WorldToRig(rig) * Inverse(camera) * Scaling(rig.Scale);
        const Quat q = QuatFromMatrix(e);
        const float orientation[4] = {static_cast<float>(q.X), static_cast<float>(q.Y), static_cast<float>(q.Z),
                                      static_cast<float>(q.W)};
        const float position[3] = {static_cast<float>(e.M[0][3]), static_cast<float>(e.M[1][3]),
                                   static_cast<float>(e.M[2][3])};
        Rows viaFloats{};
        CHECK(BuildEyeRows(f, EyeView(rig, PoseFromXr(orientation, position)), camera, false,
                           DecomposeGameProjection(f).Tangents, &viaFloats));
        CHECK_LE(RowsError(viaFloats, f), 1e-3);
    }
    Report("robustness");
}

} // namespace

int main() {
    TestDecomposition();
    TestIdentity();
    TestMirrorParity();
    TestRig();
    TestSky();
    TestPlacement();
    TestTether();
    TestSnapTurn();
    TestFloor();
    TestSeated();
    TestRobustness();
    std::printf("oot3d_xr_math_tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
