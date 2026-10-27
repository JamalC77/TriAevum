// oot3d_xr_math.h: the pure math of the in-world VR mode (quest-research/inworld-spec.md §2.2 and §5).
//
//  - the rig R_rig (tracking space T in metres -> VR world in game units) and the eye view V_i;
//  - the decomposition of a draw's projection rows f[0..3] into Rot3ds (R), the game camera tangents and
//    the z/w rows; the per-eye replacement rows F' = blockdiag(R+, I2) * P_eye,d * V_i * M * V_g,d^-1;
//  - the mirror rule (det R against det Rot(V_g)) and the sky rows (rotation only);
//  - the rig's state functions: placement and recenter, leash/glide tether with idle re-centre, floor
//    follow (ground, swim, dive), snap turn, seated offset.
//
// No renderer, OpenXR, SDL or guest types; callers convert at the boundary. Header-only, C++20, std only.
//
// Conventions
//  - Matrices are row-major and act on column vectors (p' = M * p); Mat4::M[row][col].
//  - Game world W: game units (u), right-handed, +Y up, yaw psi = atan2(dx, dz) (0 faces +Z, +90 deg faces
//    +X, so increasing yaw turns left seen from above); binary angle 0x8000 = 180 deg. The VR world is W
//    seen through the mirror M (identical to W unless the frame is mirrored, §2.2): every rig <-> game
//    conversion goes through ToVrWorld/ToGameWorld.
//  - Tracking space T (OpenXR STAGE or LOCAL_FLOOR): metres, right-handed, +Y up, floor y = 0, forward -Z.
//    Poses are OpenXR's: orientation quaternion (x, y, z, w) and position, local -> T.
//  - Game and eye view spaces look down -Z, +X right, +Y up.
//  - f[k] (k = 0..3) are the projection rows the translated vertex programs dot with the camera-space
//    position (clip_k = dot(f[k], v)); each row holds (x, y, z, w) as Oot3dPicaVertexUniformState::Floats.
//    WorldToView arrives column-major (PicaNativeLightBinding::WorldToView): Mat4FromColumnMajor.
//  - Everything is computed in double; only the rows handed back are float.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Oot3dNativeGame::XrMath {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDefaultScale = 33.0;           // game units per metre (§5.1)
inline constexpr double kStandingEyeHeightM = 1.60;     // seated reference eye height (§5.6)
inline constexpr double kDefaultLeashM = 3.5;           // §5.3, with W1's 1.0 m landing distance
inline constexpr double kSeatedLeashM = 2.5;            // §5.6
// The head-to-Link distance of every placement, blink landing and idle re-centre (TRIAEVUM_XR_PLACE_M).
inline constexpr double kDefaultPlaceDistanceM = 1.8;
inline constexpr double kMinPlaceDistanceM = 0.8;
inline constexpr double kMaxPlaceDistanceM = 4.0;
inline constexpr double kW1PlaceDistanceM = 1.0;        // the distance the W1 leash defaults were chosen for
inline constexpr double kLeashMarginM = 0.5;            // the leash always lies this far past the landing
inline constexpr double kDefaultSnapDegrees = 45.0;     // §5.5
inline constexpr double kOffAxisTolerance = 1e-3;       // §2.2: off-axis terms above this fraction of the scale
inline constexpr double kHomogeneousTolerance = 1e-3;   // f3's x/y/w terms relative to |C|

inline double DegreesToRadians(double degrees) { return degrees * (kPi / 180.0); }
inline double RadiansToDegrees(double radians) { return radians * (180.0 / kPi); }

// Wraps to (-pi, pi]; non-finite -> 0.
inline double WrapAngle(double angle) {
    if (!std::isfinite(angle)) return 0.0;
    double wrapped = std::remainder(angle, 2.0 * kPi);  // [-pi, pi]
    if (wrapped <= -kPi) wrapped += 2.0 * kPi;
    return wrapped;
}

// Binary angles (0x8000 = 180 deg).
inline double BinAngleToRadians(int16_t bin) { return static_cast<double>(bin) * (kPi / 32768.0); }
inline int16_t RadiansToBinAngle(double radians) {
    const long value = std::lround(WrapAngle(radians) * (32768.0 / kPi));  // [-32768, 32768]
    return static_cast<int16_t>(static_cast<uint16_t>(static_cast<unsigned long>(value) & 0xFFFFUL));
}

// ---------------------------------------------------------------------------------------------------
// Vectors, matrices, quaternions, poses

struct Vec3 {
    double X = 0.0, Y = 0.0, Z = 0.0;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.X + b.X, a.Y + b.Y, a.Z + b.Z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.X - b.X, a.Y - b.Y, a.Z - b.Z}; }
inline Vec3 operator-(const Vec3& a) { return {-a.X, -a.Y, -a.Z}; }
inline Vec3 operator*(const Vec3& a, double s) { return {a.X * s, a.Y * s, a.Z * s}; }
inline Vec3 operator*(double s, const Vec3& a) { return a * s; }
inline double Dot(const Vec3& a, const Vec3& b) { return a.X * b.X + a.Y * b.Y + a.Z * b.Z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}
inline double Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
inline double LengthXZ(const Vec3& a) { return std::hypot(a.X, a.Z); }
inline double DistanceXZ(const Vec3& a, const Vec3& b) { return LengthXZ(a - b); }
inline bool IsFinite(const Vec3& a) { return std::isfinite(a.X) && std::isfinite(a.Y) && std::isfinite(a.Z); }
// The horizontal unit direction of `a`, or `fallback` when it has no usable horizontal part.
inline Vec3 NormalizedXZ(const Vec3& a, const Vec3& fallback) {
    const double length = LengthXZ(a);
    if (!IsFinite(a) || !(length > 1e-9)) return fallback;
    return {a.X / length, 0.0, a.Z / length};
}
inline double YawOf(const Vec3& direction) { return std::atan2(direction.X, direction.Z); }
inline Vec3 YawDirection(double yaw) { return {std::sin(yaw), 0.0, std::cos(yaw)}; }

using Vec4 = std::array<double, 4>;

struct Mat4 {
    std::array<std::array<double, 4>, 4> M{};
    static Mat4 Identity() {
        Mat4 result;
        for (std::size_t i = 0; i < 4; ++i) result.M[i][i] = 1.0;
        return result;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 result;
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            double sum = 0.0;
            for (std::size_t k = 0; k < 4; ++k) sum += a.M[row][k] * b.M[k][col];
            result.M[row][col] = sum;
        }
    }
    return result;
}

inline Vec4 Transform(const Mat4& m, const Vec4& v) {
    Vec4 result{};
    for (std::size_t row = 0; row < 4; ++row)
        result[row] = m.M[row][0] * v[0] + m.M[row][1] * v[1] + m.M[row][2] * v[2] + m.M[row][3] * v[3];
    return result;
}
// Affine point and direction transforms (row 3 ignored).
inline Vec3 TransformPoint(const Mat4& m, const Vec3& p) {
    return {m.M[0][0] * p.X + m.M[0][1] * p.Y + m.M[0][2] * p.Z + m.M[0][3],
            m.M[1][0] * p.X + m.M[1][1] * p.Y + m.M[1][2] * p.Z + m.M[1][3],
            m.M[2][0] * p.X + m.M[2][1] * p.Y + m.M[2][2] * p.Z + m.M[2][3]};
}
inline Vec3 TransformDirection(const Mat4& m, const Vec3& d) {
    return {m.M[0][0] * d.X + m.M[0][1] * d.Y + m.M[0][2] * d.Z,
            m.M[1][0] * d.X + m.M[1][1] * d.Y + m.M[1][2] * d.Z,
            m.M[2][0] * d.X + m.M[2][1] * d.Y + m.M[2][2] * d.Z};
}

inline Mat4 Translation(const Vec3& t) {
    Mat4 result = Mat4::Identity();
    result.M[0][3] = t.X;
    result.M[1][3] = t.Y;
    result.M[2][3] = t.Z;
    return result;
}
inline Mat4 Scaling(double s) {
    Mat4 result = Mat4::Identity();
    result.M[0][0] = result.M[1][1] = result.M[2][2] = s;
    return result;
}
// Standard right-handed rotation about +Y: RotY(t) = [[cos, 0, sin], [0, 1, 0], [-sin, 0, cos]].
inline Mat4 RotationY(double angle) {
    Mat4 result = Mat4::Identity();
    const double c = std::cos(angle), s = std::sin(angle);
    result.M[0][0] = c;
    result.M[0][2] = s;
    result.M[2][0] = -s;
    result.M[2][2] = c;
    return result;
}
// M of §2.2: the fixed world-space X mirror diag(-1, 1, 1) (identity when not mirrored).
inline Mat4 MirrorMatrix(bool mirrored) {
    Mat4 result = Mat4::Identity();
    if (mirrored) result.M[0][0] = -1.0;
    return result;
}

inline bool IsFinite(const Mat4& m) {
    for (const auto& row : m.M)
        for (double v : row)
            if (!std::isfinite(v)) return false;
    return true;
}
inline double Determinant3(const Mat4& m) {
    const auto& a = m.M;
    return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
}
inline double MaxAbs3(const Mat4& m) {
    double largest = 0.0;
    for (std::size_t r = 0; r < 3; ++r)
        for (std::size_t c = 0; c < 3; ++c) largest = std::max(largest, std::fabs(m.M[r][c]));
    return largest;
}
inline double MaxAbsDifference(const Mat4& a, const Mat4& b) {
    double largest = 0.0;
    for (std::size_t r = 0; r < 4; ++r)
        for (std::size_t c = 0; c < 4; ++c) largest = std::max(largest, std::fabs(a.M[r][c] - b.M[r][c]));
    return largest;
}
// The transpose of the 3x3 block, no translation.
inline Mat4 Transpose3(const Mat4& m) {
    Mat4 result = Mat4::Identity();
    for (std::size_t r = 0; r < 3; ++r)
        for (std::size_t c = 0; c < 3; ++c) result.M[r][c] = m.M[c][r];
    return result;
}

// Inverse of an affine matrix (last row 0 0 0 1), any 3x3 block (rotation, uniform scale, mirror).
// False for non-finite, non-affine or (near-)singular input; `out` is untouched then.
inline bool InverseAffine(const Mat4& m, Mat4* out) {
    if (!IsFinite(m)) return false;
    const auto& a = m.M;
    const double largest = MaxAbs3(m);
    if (!(largest > 0.0)) return false;
    const double rowTolerance = 1e-9 * std::max(1.0, std::fabs(a[3][3]));
    if (std::fabs(a[3][0]) > rowTolerance || std::fabs(a[3][1]) > rowTolerance ||
        std::fabs(a[3][2]) > rowTolerance || std::fabs(a[3][3] - 1.0) > 1e-9)
        return false;
    const double det = Determinant3(m);
    if (!(std::fabs(det) > 1e-9 * largest * largest * largest)) return false;
    const double inv = 1.0 / det;
    Mat4 result = Mat4::Identity();
    auto& r = result.M;
    r[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * inv;
    r[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * inv;
    r[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * inv;
    r[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * inv;
    r[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * inv;
    r[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * inv;
    r[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * inv;
    r[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * inv;
    r[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * inv;
    for (std::size_t row = 0; row < 3; ++row)
        r[row][3] = -(r[row][0] * a[0][3] + r[row][1] * a[1][3] + r[row][2] * a[2][3]);
    if (!IsFinite(result)) return false;
    *out = result;
    return true;
}

// PicaNativeLightBinding::WorldToView and friends are column-major float[16].
inline Mat4 Mat4FromColumnMajor(const std::array<float, 16>& columnMajor) {
    Mat4 result;
    for (std::size_t col = 0; col < 4; ++col)
        for (std::size_t row = 0; row < 4; ++row) result.M[row][col] = static_cast<double>(columnMajor[col * 4 + row]);
    return result;
}
inline std::array<float, 16> ToColumnMajor(const Mat4& m) {
    std::array<float, 16> result{};
    for (std::size_t col = 0; col < 4; ++col)
        for (std::size_t row = 0; row < 4; ++row) result[col * 4 + row] = static_cast<float>(m.M[row][col]);
    return result;
}

struct Quat {
    double X = 0.0, Y = 0.0, Z = 0.0, W = 1.0;
};

// Unit quaternion; zero-length or non-finite input gives identity and *valid = false.
inline Quat Normalize(const Quat& q, bool* valid = nullptr) {
    const double length = std::sqrt(q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W);
    const bool ok = std::isfinite(length) && length > 1e-12;
    if (valid != nullptr) *valid = ok;
    if (!ok) return {};
    return {q.X / length, q.Y / length, q.Z / length, q.W / length};
}
inline Quat operator*(const Quat& a, const Quat& b) {
    return {a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y, a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
            a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W, a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z};
}
inline Quat QuatFromAxisAngle(const Vec3& axis, double angle) {
    const double length = Length(axis);
    if (!(length > 1e-12) || !std::isfinite(angle)) return {};
    const double s = std::sin(angle * 0.5) / length;
    return {axis.X * s, axis.Y * s, axis.Z * s, std::cos(angle * 0.5)};
}
// Rotation matrix of a quaternion (normalised first), OpenXR/Hamilton convention.
inline Mat4 RotationMatrix(const Quat& input) {
    const Quat q = Normalize(input);
    const double xx = q.X * q.X, yy = q.Y * q.Y, zz = q.Z * q.Z;
    const double xy = q.X * q.Y, xz = q.X * q.Z, yz = q.Y * q.Z;
    const double wx = q.W * q.X, wy = q.W * q.Y, wz = q.W * q.Z;
    Mat4 result = Mat4::Identity();
    result.M[0][0] = 1.0 - 2.0 * (yy + zz);
    result.M[0][1] = 2.0 * (xy - wz);
    result.M[0][2] = 2.0 * (xz + wy);
    result.M[1][0] = 2.0 * (xy + wz);
    result.M[1][1] = 1.0 - 2.0 * (xx + zz);
    result.M[1][2] = 2.0 * (yz - wx);
    result.M[2][0] = 2.0 * (xz - wy);
    result.M[2][1] = 2.0 * (yz + wx);
    result.M[2][2] = 1.0 - 2.0 * (xx + yy);
    return result;
}
inline Vec3 Rotate(const Quat& q, const Vec3& v) { return TransformDirection(RotationMatrix(q), v); }
// Quaternion of a proper rotation's 3x3 block (Shepperd's method).
inline Quat QuatFromMatrix(const Mat4& m) {
    const auto& a = m.M;
    const double trace = a[0][0] + a[1][1] + a[2][2];
    Quat q;
    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        q = {(a[2][1] - a[1][2]) / s, (a[0][2] - a[2][0]) / s, (a[1][0] - a[0][1]) / s, 0.25 * s};
    } else if (a[0][0] > a[1][1] && a[0][0] > a[2][2]) {
        const double s = std::sqrt(1.0 + a[0][0] - a[1][1] - a[2][2]) * 2.0;
        q = {0.25 * s, (a[0][1] + a[1][0]) / s, (a[0][2] + a[2][0]) / s, (a[2][1] - a[1][2]) / s};
    } else if (a[1][1] > a[2][2]) {
        const double s = std::sqrt(1.0 + a[1][1] - a[0][0] - a[2][2]) * 2.0;
        q = {(a[0][1] + a[1][0]) / s, 0.25 * s, (a[1][2] + a[2][1]) / s, (a[0][2] - a[2][0]) / s};
    } else {
        const double s = std::sqrt(1.0 + a[2][2] - a[0][0] - a[1][1]) * 2.0;
        q = {(a[0][2] + a[2][0]) / s, (a[1][2] + a[2][1]) / s, 0.25 * s, (a[1][0] - a[0][1]) / s};
    }
    return Normalize(q);
}

// A pose local -> parent (OpenXR XrPosef): E_i is the eye's pose in T.
struct Pose {
    Quat Orientation;
    Vec3 Position;
};
// From XrPosef-style floats: orientation (x, y, z, w), position (x, y, z).
inline Pose PoseFromXr(const float orientationXyzw[4], const float position[3]) {
    Pose pose;
    const auto d = [](float v) { return static_cast<double>(v); };
    pose.Orientation = Normalize(Quat{d(orientationXyzw[0]), d(orientationXyzw[1]), d(orientationXyzw[2]),
                                      d(orientationXyzw[3])});
    pose.Position = {d(position[0]), d(position[1]), d(position[2])};
    if (!IsFinite(pose.Position)) pose.Position = {};
    return pose;
}
inline Mat4 PoseToMatrix(const Pose& pose) { return Translation(pose.Position) * RotationMatrix(pose.Orientation); }
inline Mat4 PoseInverseMatrix(const Pose& pose) {
    const Mat4 rotationT = Transpose3(RotationMatrix(pose.Orientation));
    return rotationT * Translation(-pose.Position);
}
// The head's horizontal gaze in its parent space. Near straight up or down the gaze's own heading is
// lost, and the heading blends continuously (smoothstep over |forward.xz| in [0, kGazeBlendXZ], i.e. the
// last ~5.7 deg) into Y x right = forward * up.y - up * forward.y, the horizontal direction square to the
// head's right vector: it equals the gaze heading for an unrolled head, is the up vector's heading when
// looking straight down (the reverse looking up), and is defined at exact vertical (right is horizontal
// there). A hard switch would jump by the head's roll for a 0.06 deg pitch change.
inline constexpr double kGazeBlendXZ = 0.1;
inline Vec3 HorizontalForward(const Quat& orientation) {
    const Vec3 forward = Rotate(orientation, {0.0, 0.0, -1.0});
    const Vec3 fallback{0.0, 0.0, -1.0};
    if (!IsFinite(forward)) return fallback;
    const double gazeXZ = LengthXZ(forward);
    if (gazeXZ >= kGazeBlendXZ) return NormalizedXZ(forward, fallback);
    const Vec3 up = Rotate(orientation, {0.0, 1.0, 0.0});
    const Vec3 square = NormalizedXZ(forward * up.Y - up * forward.Y, fallback);
    const double t = gazeXZ / kGazeBlendXZ;
    const double w = t * t * (3.0 - 2.0 * t);
    const Vec3 gaze = gazeXZ > 0.0 ? Vec3{forward.X / gazeXZ, 0.0, forward.Z / gazeXZ} : square;
    return NormalizedXZ(gaze * w + square * (1.0 - w), gazeXZ > 0.0 ? gaze : square);
}

// An eye's field of view as tangents (OpenXR XrFovf: angleLeft/angleDown negative).
struct EyeFov {
    double TanLeft = -1.0, TanRight = 1.0, TanUp = 1.0, TanDown = -1.0;
};
inline EyeFov FovFromAngles(double angleLeft, double angleRight, double angleUp, double angleDown) {
    return {std::tan(angleLeft), std::tan(angleRight), std::tan(angleUp), std::tan(angleDown)};
}
inline EyeFov SymmetricFov(double halfAngleRadians) {
    const double t = std::tan(halfAngleRadians);
    return {-t, t, t, -t};
}
inline bool ValidFov(const EyeFov& fov) {
    return std::isfinite(fov.TanLeft) && std::isfinite(fov.TanRight) && std::isfinite(fov.TanUp) &&
           std::isfinite(fov.TanDown) && fov.TanRight - fov.TanLeft > 1e-6 && fov.TanUp - fov.TanDown > 1e-6;
}

// ---------------------------------------------------------------------------------------------------
// The rig (§2.2, §5): R_rig = Tr(a) * RotY(psi + pi) * Sc(s) * Tr(0, y_seat, 0), T (m) -> VR world (u).
// Yaw only, so the horizon stays level; RotY(psi + pi) maps T's forward -Z onto (sin psi, 0, cos psi).
// The T point (x, -y_seat, z) lands at height a.y: a.y is the floor Link stands on.

struct Rig {
    Vec3 Anchor;                  // a, VR world (u)
    double Yaw = 0.0;             // psi (rad)
    double Scale = kDefaultScale; // s (u/m)
    double SeatY = 0.0;           // y_seat (m), 0 standing
};

inline Mat4 RigToWorld(const Rig& rig) {
    return Translation(rig.Anchor) * RotationY(rig.Yaw + kPi) * Scaling(rig.Scale) *
           Translation({0.0, rig.SeatY, 0.0});
}
inline Mat4 WorldToRig(const Rig& rig) {
    return Translation({0.0, -rig.SeatY, 0.0}) * Scaling(1.0 / rig.Scale) * RotationY(-(rig.Yaw + kPi)) *
           Translation(-rig.Anchor);
}
inline Vec3 TrackingToWorld(const Rig& rig, const Vec3& pointT) { return TransformPoint(RigToWorld(rig), pointT); }
inline Vec3 WorldToTracking(const Rig& rig, const Vec3& pointW) { return TransformPoint(WorldToRig(rig), pointW); }
inline Vec3 TrackingDirectionToWorld(const Rig& rig, const Vec3& directionT) {
    return TransformDirection(RotationY(rig.Yaw + kPi), directionT);
}
inline Vec3 WorldDirectionToTracking(const Rig& rig, const Vec3& directionW) {
    return TransformDirection(RotationY(-(rig.Yaw + kPi)), directionW);
}
// The world yaw of a T direction: yaw_T + psi + pi.
inline double TrackingYawToWorld(const Rig& rig, double yawT) { return WrapAngle(yawT + rig.Yaw + kPi); }

// V_i = Sc(s) * E_i^-1 * R_rig^-1: VR world (u) -> eye i view space (u). Rigid (unit scale).
inline Mat4 EyeView(const Rig& rig, const Pose& eyeT) {
    return Scaling(rig.Scale) * PoseInverseMatrix(eyeT) * WorldToRig(rig);
}

// The head in the VR world (camera write, §4.2; head-in-geometry probes, §4.4).
struct WorldHead {
    Vec3 Position;  // u
    Vec3 Forward;   // unit
    double Yaw = 0.0;
    double Pitch = 0.0;
};
inline WorldHead HeadToWorld(const Rig& rig, const Pose& headT) {
    WorldHead head;
    head.Position = TrackingToWorld(rig, headT.Position);
    head.Forward = TrackingDirectionToWorld(rig, Rotate(headT.Orientation, {0.0, 0.0, -1.0}));
    head.Yaw = YawOf(head.Forward);
    head.Pitch = std::asin(std::clamp(head.Forward.Y, -1.0, 1.0));
    return head;
}

// ---------------------------------------------------------------------------------------------------
// Mirror (§2.2): mirrored = det(R) * det(Rot(V_g)) < 0, evaluated once per sample on the frame camera.
// M = diag(-1, 1, 1) then sits between the game world and the VR world.

inline Vec3 ToVrWorld(const Vec3& gameWorld, bool mirrored) {
    return mirrored ? Vec3{-gameWorld.X, gameWorld.Y, gameWorld.Z} : gameWorld;
}
inline Vec3 ToGameWorld(const Vec3& vrWorld, bool mirrored) { return ToVrWorld(vrWorld, mirrored); }
inline double YawToVrWorld(double gameYaw, bool mirrored) { return mirrored ? WrapAngle(-gameYaw) : gameYaw; }
inline double YawToGameWorld(double vrYaw, bool mirrored) { return YawToVrWorld(vrYaw, mirrored); }
inline WorldHead HeadToGameWorld(const WorldHead& vrHead, bool mirrored) {
    WorldHead head = vrHead;
    head.Position = ToGameWorld(vrHead.Position, mirrored);
    head.Forward = ToGameWorld(vrHead.Forward, mirrored);
    head.Yaw = YawToGameWorld(vrHead.Yaw, mirrored);
    return head;
}

inline bool MirroredFrame(int detR, double detRotVg) {
    if (detR == 0 || !std::isfinite(detRotVg) || detRotVg == 0.0) return false;
    return (detR < 0) != (detRotVg < 0.0);
}

// What the W0 trace logs about a draw camera V_g,d: det sign, uniform scale, orthonormality.
struct ViewInfo {
    bool Valid = false;
    int DetSign = 0;           // sign of det(Rot(V_g))
    double Scale = 0.0;        // cbrt(|det|), expected 1
    double OrthoError = 0.0;   // max |(Rot/scale)^T (Rot/scale) - I|
};
inline ViewInfo AnalyzeView(const Mat4& view) {
    ViewInfo info;
    if (!IsFinite(view)) return info;
    const double det = Determinant3(view);
    const double scale = std::cbrt(std::fabs(det));
    if (!(scale > 1e-9) || !std::isfinite(scale)) return info;
    info.Valid = true;
    info.DetSign = det < 0.0 ? -1 : 1;
    info.Scale = scale;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            double sum = 0.0;
            for (std::size_t k = 0; k < 3; ++k) sum += view.M[k][i] * view.M[k][j];
            info.OrthoError = std::max(info.OrthoError, std::fabs(sum / (scale * scale) - (i == j ? 1.0 : 0.0)));
        }
    }
    return info;
}
// Rot(V): the 3x3 block with its uniform scale removed (sign of det kept), no translation.
inline Mat4 RotationPart(const Mat4& view) {
    Mat4 result = Mat4::Identity();
    const double scale = std::cbrt(std::fabs(Determinant3(view)));
    if (!(scale > 1e-9) || !std::isfinite(scale)) return result;
    for (std::size_t r = 0; r < 3; ++r)
        for (std::size_t c = 0; c < 3; ++c) result.M[r][c] = view.M[r][c] / scale;
    return result;
}

// ---------------------------------------------------------------------------------------------------
// The draw's projection rows (§2.2)

using Rows = std::array<std::array<float, 4>, 4>;  // f[0..3], each (x, y, z, w)
using Rot2 = std::array<std::array<int, 2>, 2>;    // a 2x2 signed permutation

enum class ProjectionStatus : uint8_t {
    Ok,
    NonFinite,       // NaN or Inf in f[0..3]
    NotPerspective,  // f3 is not (0, 0, C, 0)
    NonNegativeC,    // f3 = (0, 0, C, 0) with C >= 0
    Degenerate,      // a zero x or y scale
    OffAxis,         // f0/f1 off-axis terms above kOffAxisTolerance of the scale
};

inline const char* ProjectionStatusName(ProjectionStatus status) {
    switch (status) {
    case ProjectionStatus::Ok: return "ok";
    case ProjectionStatus::NonFinite: return "non_finite";
    case ProjectionStatus::NotPerspective: return "not_perspective";
    case ProjectionStatus::NonNegativeC: return "c_non_negative";
    case ProjectionStatus::Degenerate: return "degenerate";
    case ProjectionStatus::OffAxis: return "off_axis";
    }
    return "unknown";
}

// [f0; f1] = R * [px; py], px = (sx, 0, ox, 0), py = (0, sy, oy, 0), sx, sy > 0; f2 = (0, 0, A, B) and
// f3 = (0, 0, C, 0) are kept as the draw's. Tangents are the game camera's (tL < tR, tD < tU).
struct GameProjection {
    ProjectionStatus Status = ProjectionStatus::NonFinite;
    Rot2 Rot3ds{};  // R
    int DetR = 0;
    double Sx = 0.0, Sy = 0.0, Ox = 0.0, Oy = 0.0;
    double C = 0.0;
    Vec4 Row2{};  // f2 as given
    Vec4 Row3{};  // f3 as given
    EyeFov Tangents;
    double OffAxis = 0.0;       // largest off-axis term of f0/f1 relative to the scale (diagnostic)
    double DepthOffAxis = 0.0;  // largest x/y term of f2 relative to its norm (diagnostic, not a failure)
    bool Ok() const { return Status == ProjectionStatus::Ok; }
};

inline int Det2(const Rot2& r) { return r[0][0] * r[1][1] - r[0][1] * r[1][0]; }

inline GameProjection DecomposeGameProjection(const Rows& f) {
    GameProjection g;
    for (const auto& row : f)
        for (float v : row)
            if (!std::isfinite(v)) return g;  // NonFinite
    const auto row = [&f](std::size_t k) -> Vec4 {
        return {static_cast<double>(f[k][0]), static_cast<double>(f[k][1]), static_cast<double>(f[k][2]),
                static_cast<double>(f[k][3])};
    };
    const Vec4 f0 = row(0), f1 = row(1);
    g.Row2 = row(2);
    g.Row3 = row(3);

    g.C = g.Row3[2];
    const double absC = std::fabs(g.C);
    if (!(absC > 1e-12) || std::fabs(g.Row3[0]) > kHomogeneousTolerance * absC ||
        std::fabs(g.Row3[1]) > kHomogeneousTolerance * absC || std::fabs(g.Row3[3]) > kHomogeneousTolerance * absC) {
        g.Status = ProjectionStatus::NotPerspective;
        return g;
    }
    if (g.C >= 0.0) {
        g.Status = ProjectionStatus::NonNegativeC;
        return g;
    }

    const auto sign = [](double v) { return v < 0.0 ? -1 : 1; };
    if (std::fabs(f0[0]) >= std::fabs(f0[1])) {
        g.Rot3ds = {{{sign(f0[0]), 0}, {0, sign(f1[1])}}};
    } else {
        g.Rot3ds = {{{0, sign(f0[1])}, {sign(f1[0]), 0}}};
    }
    g.DetR = Det2(g.Rot3ds);
    // [px; py] = R^T [f0; f1] (R is orthogonal).
    Vec4 px{}, py{};
    for (std::size_t k = 0; k < 4; ++k) {
        px[k] = g.Rot3ds[0][0] * f0[k] + g.Rot3ds[1][0] * f1[k];
        py[k] = g.Rot3ds[0][1] * f0[k] + g.Rot3ds[1][1] * f1[k];
    }
    g.Sx = px[0];
    g.Sy = py[1];
    g.Ox = px[2];
    g.Oy = py[2];
    if (!(g.Sx > 1e-6 * absC) || !(g.Sy > 1e-6 * absC)) {
        g.Status = ProjectionStatus::Degenerate;
        return g;
    }
    const double scale = std::max(g.Sx, g.Sy);
    g.OffAxis = std::max({std::fabs(px[1]), std::fabs(px[3]), std::fabs(py[0]), std::fabs(py[3])}) / scale;
    const double depthNorm = std::max({std::fabs(g.Row2[2]), std::fabs(g.Row2[3]), 1e-30});
    g.DepthOffAxis = std::max(std::fabs(g.Row2[0]), std::fabs(g.Row2[1])) / depthNorm;
    // Tangents: tR - tL = -2C/sx, tR + tL = -ox (tR - tL) / C = 2 ox / sx; same for y.
    g.Tangents.TanRight = (g.Ox - g.C) / g.Sx;
    g.Tangents.TanLeft = (g.Ox + g.C) / g.Sx;
    g.Tangents.TanUp = (g.Oy - g.C) / g.Sy;
    g.Tangents.TanDown = (g.Oy + g.C) / g.Sy;
    g.Status = g.OffAxis > kOffAxisTolerance ? ProjectionStatus::OffAxis : ProjectionStatus::Ok;
    return g;
}

// The mirror rule on the frame camera: its draw's R against det(Rot(V_g)) of its WorldToView.
inline bool MirroredFrame(const GameProjection& frameDraw, const Mat4& frameView) {
    return frameDraw.Ok() && MirroredFrame(frameDraw.DetR, Determinant3(frameView));
}

// R+ = det R < 0 ? R * diag(-1, 1) : R (always det +1: a pure 3DS screen rotation).
inline Rot2 RotPlus(const Rot2& r) {
    if (Det2(r) >= 0) return r;
    return {{{-r[0][0], r[0][1]}, {-r[1][0], r[1][1]}}};
}

// P_eye,d = [row_x; row_y; f2; f3] for any C < 0:
//   row_x = (-2C/(tR-tL), 0, -C(tR+tL)/(tR-tL), 0), row_y = (0, -2C/(tU-tD), -C(tU+tD)/(tU-tD), 0).
inline Mat4 EyeProjection(const GameProjection& g, const EyeFov& fov) {
    Mat4 p;
    const double dx = fov.TanRight - fov.TanLeft;
    const double dy = fov.TanUp - fov.TanDown;
    p.M[0] = {-2.0 * g.C / dx, 0.0, -g.C * (fov.TanRight + fov.TanLeft) / dx, 0.0};
    p.M[1] = {0.0, -2.0 * g.C / dy, -g.C * (fov.TanUp + fov.TanDown) / dy, 0.0};
    p.M[2] = g.Row2;
    p.M[3] = g.Row3;
    return p;
}

// The per-eye part shared by every draw with the same camera (cache per distinct V_g,d and eye):
//   full: V_i * M * V_g,d^-1          sky: Rot(V_i) * M * Rot(V_g,d)^-1  (translations dropped)
inline Mat4 EyeChain(const Mat4& eyeView, const Mat4& gameViewInverse, bool mirrored) {
    return eyeView * MirrorMatrix(mirrored) * gameViewInverse;
}
// The spec writes Rot(V_g,d)^T, which is the inverse only while V_g,d is a scaled rotation. A sample whose
// WorldToView was lerped component-wise (oot3d_native_pica_visual_frame's non-rigid fallback) is not:
// its axes shrink by cos(theta/2) across the turn, and the transpose then drifts the sky against the
// world (0.13 deg at a 6 deg step). The true inverse of the scale-normalised block equals the transpose
// for every rigid camera and stays exact for the lerped one, like the full chain's InverseAffine.
// False (out untouched) for a non-affine, non-finite or (near-)singular V_g,d or V_i, with the same
// conditioning test as the full chain, so a collapsed camera axis cannot pass as a sky camera.
inline bool SkyChain(const Mat4& eyeView, const Mat4& gameView, bool mirrored, Mat4* out) {
    Mat4 gameViewInverse, eyeViewInverse;
    if (!InverseAffine(gameView, &gameViewInverse) || !InverseAffine(eyeView, &eyeViewInverse)) return false;
    Mat4 gameRotationInverse;
    if (!InverseAffine(RotationPart(gameView), &gameRotationInverse)) return false;
    const Mat4 chain = RotationPart(eyeView) * MirrorMatrix(mirrored) * gameRotationInverse;
    if (!IsFinite(chain)) return false;
    *out = chain;
    return true;
}

// F' = blockdiag(R+, I2) * P_eye,d * chain, written as float rows. False (out untouched) when the
// projection is not Ok, the FOV is invalid or the result is not finite.
inline bool ComposeEyeRows(const GameProjection& g, const EyeFov& fov, const Mat4& chain, Rows* out) {
    if (!g.Ok() || !ValidFov(fov) || !IsFinite(chain)) return false;
    const Mat4 x = EyeProjection(g, fov) * chain;
    const Rot2 rp = RotPlus(g.Rot3ds);
    Rows result{};
    for (std::size_t k = 0; k < 4; ++k) {
        const double r0 = rp[0][0] * x.M[0][k] + rp[0][1] * x.M[1][k];
        const double r1 = rp[1][0] * x.M[0][k] + rp[1][1] * x.M[1][k];
        const double values[4] = {r0, r1, x.M[2][k], x.M[3][k]};
        for (std::size_t row = 0; row < 4; ++row) {
            if (!std::isfinite(values[row])) return false;
            result[row][k] = static_cast<float>(values[row]);
        }
    }
    *out = result;
    return true;
}

// One-shot forms (the eye pass caches the chain instead).
inline bool BuildEyeRows(const Rows& f, const Mat4& eyeView, const Mat4& gameView, bool mirrored, const EyeFov& fov,
                         Rows* out) {
    Mat4 gameViewInverse;
    if (!InverseAffine(gameView, &gameViewInverse)) return false;
    return ComposeEyeRows(DecomposeGameProjection(f), fov, EyeChain(eyeView, gameViewInverse, mirrored), out);
}
inline bool BuildSkyRows(const Rows& f, const Mat4& eyeView, const Mat4& gameView, bool mirrored, const EyeFov& fov,
                         Rows* out) {
    Mat4 chain;
    if (!SkyChain(eyeView, gameView, mirrored, &chain)) return false;
    return ComposeEyeRows(DecomposeGameProjection(f), fov, chain, out);
}

// ---------------------------------------------------------------------------------------------------
// Rig events (TRIAEVUM_XR_RIG event=...)

enum class RigEvent : uint8_t { None, Place, Blink, Recenter, Turn, Door, Idle, Glide };
inline const char* RigEventName(RigEvent event) {
    switch (event) {
    case RigEvent::None: return "none";
    case RigEvent::Place: return "place";
    case RigEvent::Blink: return "blink";
    case RigEvent::Recenter: return "recenter";
    case RigEvent::Turn: return "turn";
    case RigEvent::Door: return "door";
    case RigEvent::Idle: return "idle";
    case RigEvent::Glide: return "glide";
    }
    return "unknown";
}

// The anchor that puts T point `linkT` (x, z) onto world point `linkWorld` under `rig`'s yaw and scale;
// a.y = linkWorld.y (Link's floor at the virtual floor).
inline Vec3 AnchorForLink(const Rig& rig, const Vec3& linkWorld, const Vec3& linkT) {
    const Vec3 offset = TransformDirection(RotationY(rig.Yaw + kPi), Vec3{linkT.X, 0.0, linkT.Z} * rig.Scale);
    return {linkWorld.X - offset.X, linkWorld.Y, linkWorld.Z - offset.Z};
}

// ---------------------------------------------------------------------------------------------------
// Placement and recenter (§5.2, §4.4)
//
// Link goes PlaceDistanceM (TRIAEVUM_XR_PLACE_M, default 1.8 m) ahead of Jamal's head, horizontally, with
// psi chosen so he faces away from Jamal (Jamal behind him), Link's floor at the real floor. The direction
// is the line from the head through the STAGE centre c (Link lands on, short of or past c); within 0.6 m
// of c it is the horizontal gaze. A small boundary (a stationary 1 x 1 m one) therefore has Link standing
// outside it, which is intended: he is never placed on top of Jamal. A probe distance (W2, §4.4) shortens
// the distance (never below 0.7 m).
//
// W1 (before the knob): Link at c with d0 = Jamal's distance to c capped at 2 m, 1.0 m ahead of the gaze
// near c; Jamal found 1.0 m too close.

// The knob's value: clamped to [0.8, 4.0] m; non-finite -> the default.
inline double ClampPlaceDistanceM(double value) {
    if (!std::isfinite(value)) return kDefaultPlaceDistanceM;
    return std::clamp(value, kMinPlaceDistanceM, kMaxPlaceDistanceM);
}

struct PlacementConfig {
    double Scale = kDefaultScale;
    double SeatY = 0.0;
    double NearCentreM = 0.6;
    double PlaceDistanceM = kDefaultPlaceDistanceM;
    double MinProbeM = 0.7;
};
struct PlacementInput {
    Vec3 LinkWorld;              // VR world (ToVrWorld), y = the floor Link stands on
    double LinkFacingYaw = 0.0;  // VR world (YawToVrWorld)
    Pose Head;                   // head pose in T
    Vec3 CentreT;                // c: STAGE origin, or the head at the first placement without bounds
    double ProbeDistanceM = std::numeric_limits<double>::infinity();
};
struct PlacementResult {
    bool Valid = false;
    Rig NewRig;
    Vec3 LinkT;      // where Link lands in T (y = the virtual floor, -y_seat)
    Vec3 FacingT;    // Link's facing in T (unit, horizontal), pointing away from Jamal
    double DistanceM = 0.0;
    bool NearCentre = false;
};

inline PlacementResult SolvePlacement(const PlacementInput& input, const PlacementConfig& config) {
    PlacementResult result;
    if (!IsFinite(input.LinkWorld) || !std::isfinite(input.LinkFacingYaw) || !IsFinite(input.Head.Position) ||
        !IsFinite(input.CentreT) || !(config.Scale > 0.0) || !std::isfinite(config.Scale))
        return result;
    const Vec3 head{input.Head.Position.X, 0.0, input.Head.Position.Z};
    const Vec3 toCentre{input.CentreT.X - head.X, 0.0, input.CentreT.Z - head.Z};
    const double centreDistance = LengthXZ(toCentre);
    Vec3 direction;
    // The near-centre rule also covers a head too close to c to give a direction, whatever NearCentreM
    // says (0, negative or NaN would otherwise divide by a zero distance).
    if (centreDistance < config.NearCentreM || !(centreDistance > 1e-6)) {
        result.NearCentre = true;
        direction = HorizontalForward(input.Head.Orientation);
    } else {
        direction = toCentre * (1.0 / centreDistance);
    }
    // A distance that is not positive would put Link on Jamal's head: invalid, like a non-finite one.
    double distance = config.PlaceDistanceM > 0.0 ? config.PlaceDistanceM : std::numeric_limits<double>::quiet_NaN();
    if (!std::isnan(input.ProbeDistanceM)) distance = std::min(distance, std::max(input.ProbeDistanceM, config.MinProbeM));

    Rig rig;
    rig.Scale = config.Scale;
    rig.SeatY = config.SeatY;
    // RotY(psi + pi) adds psi + pi to a T yaw: yaw_T(direction) + psi + pi = Link's facing yaw.
    rig.Yaw = WrapAngle(input.LinkFacingYaw - kPi - YawOf(direction));
    result.LinkT = {head.X + direction.X * distance, -config.SeatY, head.Z + direction.Z * distance};
    rig.Anchor = AnchorForLink(rig, input.LinkWorld, result.LinkT);
    result.NewRig = rig;
    result.FacingT = direction;
    result.DistanceM = distance;
    // Valid only when everything came out finite (a non-finite config distance would poison the rig).
    result.Valid = IsFinite(rig.Anchor) && std::isfinite(rig.Yaw) && std::isfinite(rig.SeatY) &&
                   IsFinite(result.LinkT) && IsFinite(direction) && std::isfinite(distance);
    return result;
}

// ---------------------------------------------------------------------------------------------------
// Following Link (§5.3): free zone, leash blink, idle re-centre, glide option.

struct PlayArea {
    double HalfX = 0.0, HalfZ = 0.0;
    bool Bounded = false;
};
// r_room = max(min(half extents) - 0.3 m, 0.6 m); 1.0 m without bounds.
inline double RoomRadius(const PlayArea& area) {
    if (!area.Bounded) return 1.0;
    return std::max(std::min(area.HalfX, area.HalfZ) - 0.3, 0.6);
}
// Horizontal distance of `pointT` outside the play-area rectangle centred on `centreT` (0 inside, or
// without bounds).
inline double OutsideDistance(const PlayArea& area, const Vec3& pointT, const Vec3& centreT) {
    if (!area.Bounded) return 0.0;
    const double dx = std::max(std::fabs(pointT.X - centreT.X) - area.HalfX, 0.0);
    const double dz = std::max(std::fabs(pointT.Z - centreT.Z) - area.HalfZ, 0.0);
    return std::hypot(dx, dz);
}

inline double DefaultLeashM(bool seated) { return seated ? kSeatedLeashM : kDefaultLeashM; }
// The leash for a placement distance when TRIAEVUM_XR_LEASH_M is not set: W1's running room past the
// landing point (2.5 m standing, 1.5 m seated, about one blink per 0.7 s of running at ~3.6 m/s) kept.
inline double DefaultLeashForPlaceM(double placeDistanceM, bool seated) {
    return DefaultLeashM(seated) + (ClampPlaceDistanceM(placeDistanceM) - kW1PlaceDistanceM);
}

enum class TetherMode : uint8_t { Leash, Glide };

struct TetherConfig {
    TetherMode Mode = TetherMode::Leash;
    double LeashM = kDefaultLeashM;   // raised to PlaceDistanceM + kLeashMarginM when shorter (EffectiveLeashM)
    double OutsideAreaM = 2.0;        // Link beyond the play area by this much (past Jamal's own excursion)
    double PlaceDistanceM = kDefaultPlaceDistanceM;  // a blink lands Link this far ahead of the head
    double IdleSeconds = 0.75;
    double IdleRadiusFraction = 0.7;  // idle re-centre when |l - c| > fraction * r_room
    double StillSpeedMps = 0.1;
    double CentreClearM = 0.6;        // no idle re-centre onto Jamal standing at c
    double TeleportSpeedMps = 30.0;   // a faster step is a teleport: no travel direction from it
    // Glide: the anchor takes the excess beyond r_room, but never faster than this. Running Link (~3.6 m/s)
    // glides exactly; an excess that appears at once (a snap turn pivoting on the head, a near-centre
    // placement ahead of the gaze, a hookshot pull) is worked off over a few frames
    // instead of jumping the world by metres in one unannounced frame.
    double GlideMaxSpeedMps = 8.0;
    PlayArea Area;
    Vec3 CentreT;
};

// The landing distance in use: PlaceDistanceM, or the default when it is not a positive finite value.
inline double TetherPlaceDistanceM(const TetherConfig& config) {
    return std::isfinite(config.PlaceDistanceM) && config.PlaceDistanceM > 0.0 ? config.PlaceDistanceM
                                                                               : kDefaultPlaceDistanceM;
}
// The leash never lies inside the landing distance (a blink would repeat every update).
inline double EffectiveLeashM(const TetherConfig& config) {
    const double place = TetherPlaceDistanceM(config);
    return std::isfinite(config.LeashM) ? std::max(config.LeashM, place + kLeashMarginM) : place + kLeashMarginM;
}
// The play-area rule's distance (Link this far outside the area past Jamal's own excursion blinks). With
// a small boundary the placement distance alone puts Link outside it, so the rule never fires before Link
// could be a leash away from c, nor inside the landing distance (OutsideDistance is 1-Lipschitz: a blink
// landing is at most the landing distance farther out than the head).
inline double OutsideLimitM(const TetherConfig& config) {
    double limit = std::max(config.OutsideAreaM, TetherPlaceDistanceM(config) + kLeashMarginM);
    if (config.Area.Bounded)
        limit = std::max(limit, EffectiveLeashM(config) - std::min(config.Area.HalfX, config.Area.HalfZ));
    return limit;
}
struct TetherState {
    bool HasLast = false;
    Vec3 LastLinkWorld;
    double StillSeconds = 0.0;
    bool HasTravel = false;
    Vec3 TravelWorld;  // last horizontal travel direction of Link (VR world, unit)
};
struct TetherStep {
    RigEvent Event = RigEvent::None;
    Rig NewRig;
    Vec3 LinkT;                 // Link in T after the step
    double HeadDistanceM = 0.0; // Link to head, horizontal, after the step
    double AnchorSpeedMps = 0.0;// glide speed
};

// One guest update. `linkWorld` and `linkFacingYaw` are VR world; `headT` is the head position in T;
// dt in seconds. Rig changes from outside (placement, turn) need no reset: velocity is tracked in world.
inline TetherStep UpdateTether(TetherState& state, const TetherConfig& config, const Rig& rig, const Vec3& linkWorld,
                               double linkFacingYaw, const Vec3& headT, double dt) {
    TetherStep step;
    step.NewRig = rig;
    if (!IsFinite(linkWorld) || !IsFinite(headT) || !(rig.Scale > 0.0)) {
        step.LinkT = {};
        return step;
    }
    const double safeDt = std::isfinite(dt) && dt > 0.0 ? dt : 0.0;

    // Link's motion (world u -> m/s). "Still" is 3D: a ladder or vine climb, a jump in place or a fall is
    // motion and never starts the idle re-centre. The travel direction comes from horizontal motion only.
    if (state.HasLast && safeDt > 0.0) {
        const Vec3 delta = linkWorld - state.LastLinkWorld;
        const double speedMps = Length(delta) / rig.Scale / safeDt;
        const double horizontalMps = LengthXZ(delta) / rig.Scale / safeDt;
        if (speedMps > config.TeleportSpeedMps) {
            state.StillSeconds = 0.0;
        } else if (speedMps >= config.StillSpeedMps) {
            if (horizontalMps >= config.StillSpeedMps) {
                state.TravelWorld = NormalizedXZ(delta, YawDirection(linkFacingYaw));
                state.HasTravel = true;
            }
            state.StillSeconds = 0.0;
        } else {
            state.StillSeconds += safeDt;
        }
    }
    state.LastLinkWorld = linkWorld;
    state.HasLast = true;

    const Vec3 linkT = WorldToTracking(rig, linkWorld);
    const double headDistance = DistanceXZ(linkT, headT);
    const double linkOutside = OutsideDistance(config.Area, linkT, config.CentreT);
    const double headOutside = OutsideDistance(config.Area, headT, config.CentreT);
    const double place = TetherPlaceDistanceM(config);
    const double leash = EffectiveLeashM(config);
    step.LinkT = linkT;
    step.HeadDistanceM = headDistance;

    // Leash blink: Link the placement distance ahead of the head along his travel, psi unchanged.
    if (headDistance > leash || linkOutside > OutsideLimitM(config) + headOutside) {
        const Vec3 travelW = state.HasTravel ? state.TravelWorld : YawDirection(linkFacingYaw);
        const Vec3 travelT = NormalizedXZ(WorldDirectionToTracking(rig, travelW), {0.0, 0.0, -1.0});
        const Vec3 newLinkT{headT.X + travelT.X * place, linkT.Y, headT.Z + travelT.Z * place};
        step.NewRig.Anchor = AnchorForLink(rig, linkWorld, newLinkT);
        step.NewRig.Anchor.Y = rig.Anchor.Y;  // the floor rule owns a.y
        step.Event = RigEvent::Blink;
        step.LinkT = newLinkT;
        step.HeadDistanceM = DistanceXZ(newLinkT, headT);
        state.StillSeconds = 0.0;
        return step;
    }

    const double roomRadius = RoomRadius(config.Area);
    const double centreDistance = DistanceXZ(linkT, config.CentreT);
    // Glide keeps Link within max(r_room, placement distance) of c: a placement may stand him that far.
    const double glideRadius = std::max(roomRadius, place);
    if (config.Mode == TetherMode::Glide && centreDistance > glideRadius) {
        // Move the anchor by the excess beyond the radius (translation only), at most GlideMaxSpeedMps * dt.
        const double excess = centreDistance - glideRadius;
        const double maxShift =
            std::isfinite(config.GlideMaxSpeedMps) && config.GlideMaxSpeedMps > 0.0
                ? config.GlideMaxSpeedMps * safeDt
                : excess;
        const double shift = std::min(excess, maxShift);
        if (!(shift > 0.0)) return step;  // no time has passed: nothing moves
        const Vec3 outward{(linkT.X - config.CentreT.X) / centreDistance, 0.0,
                           (linkT.Z - config.CentreT.Z) / centreDistance};
        const Vec3 shiftT = outward * shift;
        const Vec3 shiftW = TransformDirection(RotationY(rig.Yaw + kPi), shiftT * rig.Scale);
        step.NewRig.Anchor = {rig.Anchor.X + shiftW.X, rig.Anchor.Y, rig.Anchor.Z + shiftW.Z};
        step.Event = RigEvent::Glide;
        step.LinkT = {linkT.X - shiftT.X, linkT.Y, linkT.Z - shiftT.Z};
        step.HeadDistanceM = DistanceXZ(step.LinkT, headT);
        step.AnchorSpeedMps = safeDt > 0.0 ? shift / safeDt : 0.0;
        return step;
    }

    // Idle re-centre: Link still and far from c, and c at least the placement distance from Jamal (never
    // closer to him than a placement; a stationary boundary, where c is always nearer, never re-centres)
    // and inside the leash (else the leash would blink him straight back).
    const double headCentre = DistanceXZ(headT, config.CentreT);
    if (state.StillSeconds >= config.IdleSeconds && centreDistance > config.IdleRadiusFraction * roomRadius &&
        headCentre >= std::max(config.CentreClearM, place) && headCentre < leash) {
        const Vec3 newLinkT{config.CentreT.X, linkT.Y, config.CentreT.Z};
        step.NewRig.Anchor = AnchorForLink(rig, linkWorld, newLinkT);
        step.NewRig.Anchor.Y = rig.Anchor.Y;
        step.Event = RigEvent::Idle;
        step.LinkT = newLinkT;
        step.HeadDistanceM = DistanceXZ(newLinkT, headT);
        state.StillSeconds = 0.0;
    }
    return step;
}

// ---------------------------------------------------------------------------------------------------
// Floor (§5.4): a.y follows the ground only while Link is grounded (hysteresis, glide, blink on large
// changes), the water surface while swimming, Link while diving.

enum class FloorMode : uint8_t { Ground, Swim, Dive };

struct FloorConfig {
    double GroundedEpsilonU = 2.0;     // playerY - floorHeight < 2.0 u (TopScreen's test)
    double HysteresisM = 0.15;
    double GlideTauS = 0.25;
    double BlinkThresholdM = 1.5;
    double SettleM = 0.005;            // a glide ends this close to its target
    double SwimHeadAboveWaterM = 0.45;
    double WaterValidRangeM = 1.5;
    // A camera reading that repeats (within this) the last reading the 1.5 m range test accepted stays
    // valid however deep Link dives: the surface does not move, and without it a dive below 1.5 m would
    // invalidate w and the low-passed fallback would follow Link down, so the dive rule could never fire.
    // Only the range test re-anchors that reference, so a reading creeping 0.09 m per update cannot walk
    // away from Link: it is accepted at most WaterValidRangeM + WaterStableM from where Link was.
    double WaterStableM = 0.1;
    double WaterLowPassTauS = 0.5;
    double DiveDepthM = 1.0;           // Link below w - 1 m ...
    double DiveEnterSeconds = 0.5;     // ... for 0.5 s starts a dive
    double DiveExitDepthM = 0.25;      // back within 0.25 m of the surface ends it
    double DiveTauS = 0.5;
    double DiveHeadAboveLinkM = 0.45;
};
struct FloorInput {
    double LinkY = 0.0;                                       // VR world (u)
    double FloorY = std::numeric_limits<double>::quiet_NaN(); // floor under Link (u), NaN if none
    bool Hanging = false;                                     // 0x2000
    bool Climbing = false;
    bool Swimming = false;                                    // flags1 0x08000000
    double WaterY = std::numeric_limits<double>::quiet_NaN(); // camera +0x15C (u), NaN if unknown
    double EyeHeightM = 1.6;  // the head's height over the virtual floor at recenter (head_T.y + y_seat)
    double Scale = kDefaultScale;
    double Dt = 0.0;
};
struct FloorState {
    bool Initialized = false;
    FloorMode Mode = FloorMode::Ground;
    double AnchorY = 0.0;
    double TargetY = 0.0;
    bool Gliding = false;
    bool LowPassValid = false;
    double LowPassY = 0.0;
    bool HasWater = false;
    double WaterY = 0.0;  // last water height used
    bool HasCameraWater = false;
    double CameraWaterY = 0.0;  // last camera +0x15C reading the range test accepted (the stable reference)
    double BelowSeconds = 0.0;
    bool FromDive = false;      // the swim glide that continues a dive follow: never a blink
};
struct FloorStep {
    double AnchorY = 0.0;
    bool Blink = false;
    bool Gliding = false;
    double SpeedMps = 0.0;  // |d a.y / dt|
    FloorMode Mode = FloorMode::Ground;
};

// After a placement: a.y as placed, nothing in flight.
inline void ResetFloor(FloorState& state, double anchorY) {
    state = FloorState{};
    state.Initialized = true;
    state.AnchorY = anchorY;
    state.TargetY = anchorY;
}

namespace Detail {
inline double Approach(double current, double target, double dt, double tau) {
    if (!(tau > 0.0)) return target;
    return current + (target - current) * (1.0 - std::exp(-dt / tau));
}
// Hysteresis, then glide (tau) or blink; `follow` skips the hysteresis (dive).
inline void MoveAnchorY(FloorState& state, FloorStep& step, double target, double dt, double tau, double hysteresis,
                        double blink, double settle, bool follow) {
    if (!std::isfinite(target)) return;  // hold: a non-finite target must never reach a.y
    state.TargetY = target;
    const double delta = target - state.AnchorY;
    if (std::fabs(delta) >= blink) {
        state.AnchorY = target;
        state.Gliding = false;
        step.Blink = true;
        return;
    }
    if (!state.Gliding && (follow || std::fabs(delta) > hysteresis)) state.Gliding = true;
    if (!state.Gliding) return;
    state.AnchorY = Approach(state.AnchorY, target, dt, tau);
    if (std::fabs(target - state.AnchorY) <= settle && !follow) {
        state.AnchorY = target;
        state.Gliding = false;
    }
}
} // namespace Detail

inline FloorStep UpdateFloor(FloorState& state, const FloorConfig& config, const FloorInput& input) {
    FloorStep step;
    const double s = std::isfinite(input.Scale) && input.Scale > 0.0 ? input.Scale : kDefaultScale;
    const double dt = std::isfinite(input.Dt) && input.Dt > 0.0 ? input.Dt : 0.0;
    const double seed =
        std::isfinite(input.FloorY) ? input.FloorY : (std::isfinite(input.LinkY) ? input.LinkY : 0.0);
    bool recovered = false;
    if (!state.Initialized) {
        ResetFloor(state, seed);
    } else if (!std::isfinite(state.AnchorY)) {
        // A poisoned state (e.g. ResetFloor(NaN) by the caller) recovers onto the floor: a blink.
        ResetFloor(state, seed);
        recovered = true;
    }
    if (!std::isfinite(state.TargetY)) state.TargetY = state.AnchorY;
    const double previous = state.AnchorY;
    const double hysteresis = config.HysteresisM * s;
    const double blink = config.BlinkThresholdM * s;
    const double settle = config.SettleM * s;

    // Hold on a non-finite Link y or eye height (the swim and dive targets use the eye height).
    if (!std::isfinite(input.LinkY) || !std::isfinite(input.EyeHeightM)) {
        step.Blink = recovered;
        step.AnchorY = state.AnchorY;
        step.Mode = state.Mode;
        step.Gliding = state.Gliding;
        return step;
    }

    // Mode changes.
    if (!input.Swimming) {
        state.Mode = FloorMode::Ground;
        state.BelowSeconds = 0.0;
        state.LowPassValid = false;
        state.HasCameraWater = false;
        state.FromDive = false;
    } else if (state.Mode == FloorMode::Ground) {
        state.Mode = FloorMode::Swim;
        state.BelowSeconds = 0.0;
    }

    if (state.Mode == FloorMode::Ground) {
        const bool grounded = std::isfinite(input.FloorY) && input.LinkY - input.FloorY < config.GroundedEpsilonU &&
                              !input.Hanging && !input.Climbing;
        const double target = grounded ? input.FloorY : state.TargetY;  // hold while airborne/hanging/climbing
        Detail::MoveAnchorY(state, step, target, dt, config.GlideTauS, hysteresis, blink, settle, false);
    } else {
        // The water surface: camera +0x15C when plausible, else Link's low-passed y.
        if (!state.LowPassValid) {
            state.LowPassY = input.LinkY;
            state.LowPassValid = true;
        } else {
            state.LowPassY = Detail::Approach(state.LowPassY, input.LinkY, dt, config.WaterLowPassTauS);
        }
        const bool waterFinite = std::isfinite(input.WaterY);
        const bool inRange = waterFinite && std::fabs(input.WaterY - input.LinkY) <= config.WaterValidRangeM * s;
        const bool stable = waterFinite && state.HasCameraWater &&
                            std::fabs(input.WaterY - state.CameraWaterY) <= config.WaterStableM * s;
        if (inRange) {
            state.CameraWaterY = input.WaterY;  // only the range test moves the stable reference
            state.HasCameraWater = true;
        }
        if (inRange || stable) {
            state.WaterY = input.WaterY;
            state.HasWater = true;
        } else if (state.Mode == FloorMode::Swim) {
            state.WaterY = state.LowPassY;
            state.HasWater = true;
        }
        const double water = state.WaterY;

        if (state.Mode == FloorMode::Swim) {
            if (input.LinkY < water - config.DiveDepthM * s) {
                state.BelowSeconds += dt;
            } else {
                state.BelowSeconds = 0.0;
            }
            if (state.BelowSeconds >= config.DiveEnterSeconds) {
                state.Mode = FloorMode::Dive;
                state.FromDive = false;
            }
        } else if (input.LinkY >= water - config.DiveExitDepthM * s) {
            state.Mode = FloorMode::Swim;
            state.BelowSeconds = 0.0;
            // The dive follow lags Link by about v * tau; a fast ascent (>= 3 m/s) would leave more than the
            // 1.5 m blink threshold to the swim target. The swim glide that takes over continues the dive's
            // motion, so it never blinks (until it settles).
            state.FromDive = state.Gliding;
        }

        if (state.Mode == FloorMode::Swim) {
            const double target = water + s * (config.SwimHeadAboveWaterM - input.EyeHeightM);
            Detail::MoveAnchorY(state, step, target, dt, config.GlideTauS, hysteresis,
                                state.FromDive ? std::numeric_limits<double>::infinity() : blink, settle, false);
            if (!state.Gliding) state.FromDive = false;
        } else {
            // Continuous follow: no hysteresis, never a blink.
            const double target = input.LinkY + s * (config.DiveHeadAboveLinkM - input.EyeHeightM);
            Detail::MoveAnchorY(state, step, target, dt, config.DiveTauS, hysteresis,
                                std::numeric_limits<double>::infinity(), settle, true);
        }
    }
    step.Blink = step.Blink || recovered;
    step.AnchorY = state.AnchorY;
    step.Gliding = state.Gliding;
    step.Mode = state.Mode;
    step.SpeedMps = dt > 0.0 && !step.Blink ? std::fabs(state.AnchorY - previous) / s / dt : 0.0;
    return step;
}

// ---------------------------------------------------------------------------------------------------
// Turning (§5.5) and seated (§5.6)

// Right stick X flick: fires once above `Trigger`, re-arms below `Rearm`. +1 right, -1 left.
struct SnapTurnDetector {
    double Trigger = 0.7;
    double Rearm = 0.3;
    bool Armed = true;
    int Update(double stickX) {
        if (!std::isfinite(stickX)) return 0;
        if (Armed && std::fabs(stickX) > Trigger) {
            Armed = false;
            return stickX > 0.0 ? 1 : -1;
        }
        if (!Armed && std::fabs(stickX) < Rearm) Armed = true;
        return 0;
    }
};

// psi' = psi + delta pivoting on the head: a'.xz = (head_w - s RotY(psi' + pi) head_T).xz, a.y kept.
// A non-finite head, angle or rig returns the rig unchanged (a lost tracking frame must not poison it).
inline Rig ApplyYawTurn(const Rig& rig, const Vec3& headT, double deltaYaw) {
    if (!IsFinite(headT) || !std::isfinite(deltaYaw) || !IsFinite(rig.Anchor) || !std::isfinite(rig.Yaw) ||
        !std::isfinite(rig.Scale) || !std::isfinite(rig.SeatY))
        return rig;
    const Vec3 headW = TrackingToWorld(rig, headT);
    Rig turned = rig;
    turned.Yaw = WrapAngle(rig.Yaw + deltaYaw);
    const Vec3 offset = TransformDirection(RotationY(turned.Yaw + kPi), headT * rig.Scale);
    turned.Anchor.X = headW.X - offset.X;
    turned.Anchor.Z = headW.Z - offset.Z;
    return turned;
}
// A snap of `stepRadians` to the right (+1) or left (-1). Turning right lowers the world yaw.
inline Rig SnapTurn(const Rig& rig, const Vec3& headT, int direction,
                    double stepRadians = DegreesToRadians(kDefaultSnapDegrees)) {
    if (direction == 0) return rig;
    return ApplyYawTurn(rig, headT, direction > 0 ? -stepRadians : stepRadians);
}

// y_seat = 1.60 m - seated eye height at recenter (clamped to a sane range).
inline double SeatedYOffset(double seatedEyeHeightM, double standingEyeHeightM = kStandingEyeHeightM) {
    if (!std::isfinite(seatedEyeHeightM)) return 0.0;
    return std::clamp(standingEyeHeightM - seatedEyeHeightM, -1.0, standingEyeHeightM);
}

} // namespace Oot3dNativeGame::XrMath
