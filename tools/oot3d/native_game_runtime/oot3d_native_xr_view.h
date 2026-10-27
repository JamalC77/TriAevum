// oot3d_native_xr_view.h: the picture per eye of the in-world mode (quest-research/inworld-spec.md §2.1-2.3,
// §2.7, W0 item 2, W1 day 1). Runs on the render worker inside ExecuteVisualSample:
//
//  - the frame camera of a sample (the first OpaqueWorld draw's WorldToView) and the mirror rule;
//  - the draw classes (oot3d_xr_world_logic.h) from each draw view, and the eye sample: the same draws
//    with the eye's f[0..3] rows (world, sky), the game's rows (off-target, screen, full-viewport ortho)
//    or left out (Ui, ortho overlays, unusable rows) through Oot3dPicaVisualDrawView::Omitted;
//  - TRIAEVUM_XR_VIEW statistics every 5 s, the TRIAEVUM_XR_VIEW_TRACE table (classes only, no rendering
//    change, any mode), and the PC preview eyes (TRIAEVUM_XR_PREVIEW, TRIAEVUM_XR_PREVIEW_HEAD).
#pragma once

#include "oot3d_native_pica_visual_frame.h"
#include "oot3d_xr_world_logic.h"
#include "fast/backends/stereo_views.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>

namespace Oot3dNativeGame {

// TriAevumXR logcat tag and stderr on the Quest (native-stderr.log), stderr elsewhere.
void XrWorldLog(const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

struct XrFrameCamera {
    bool Valid = false;
    std::array<float, 16> WorldToView{};  // column-major, as captured
    XrMath::Mat4 View;                     // V_g of the frame
    XrMath::GameProjection Projection;     // that draw's decomposition
    bool Mirrored = false;                 // det R * det Rot(V_g) < 0 (§2.2)
};

struct XrViewStats {
    std::array<uint64_t, XrWorld::kXrDrawClassCount> Classes{};
    uint64_t EyeSamples = 0;       // eye passes built
    uint64_t MirroredSamples = 0;
    uint64_t MirrorDisagreements = 0;  // draws whose own camera disagrees with the frame's mirror flag
    uint64_t RowFailures = 0;      // eye rows that could not be built (counted in fallback too)
    uint64_t NoFrameCamera = 0;    // samples without a frame camera
    uint64_t FallbackFrames = 0;   // consecutive eye frames with fallback draws (render safety)
    double C = 0.0;
    int DetR = 0, DetV = 0;
};

// The PC preview (§2.7): which eye is shown and where the head is.
struct XrPreviewConfig {
    bool Enabled = false;
    int Eye = 0;  // 0 left, 1 right
    XrWorld::XrPreviewHead Head;
    double IpdM = 0.064;
    double HalfFovDegrees = 50.0;
};

class XrViewRenderer {
  public:
    // From TRIAEVUM_XR_PREVIEW / _HEAD / TRIAEVUM_XR_VIEW_TRACE / TRIAEVUM_XR_SKY_PCS; `world` = the
    // immersive world mode is on. Null when none of them is.
    static std::unique_ptr<XrViewRenderer> Create(bool world);

    bool World() const { return mWorld; }
    bool Preview() const { return mPreview.Enabled; }
    const XrPreviewConfig& PreviewConfig() const { return mPreview; }
    bool TraceEnabled() const { return mTrace; }
    double Scale() const { return mScale; }
    void SetScale(double scale) { mScale = scale; }

    XrFrameCamera FindFrameCamera(const Oot3dPicaVisualFrameViewSample& sample) const;
    // The eye sample of `eye` (0 left, 1 right) in the renderer's scratch; valid until the next call for
    // the same eye. Counts the classes.
    const Oot3dPicaVisualFrameViewSample& BuildEye(const Oot3dPicaVisualFrameViewSample& sample,
                                                   const XrFrameCamera& camera, const XrWorld::XrEyeInput& eye,
                                                   bool mirrored, int eyeIndex);
    // The preview's eye (identity: the frame camera through the equivalent eye, §2.2) at time t.
    XrWorld::XrEyeInput PreviewEye(const XrFrameCamera& camera, double seconds, double aspect) const;
    // The eye of a tracked pose (§2.2 V_i = Sc(s) E_i^-1 R_rig^-1).
    static XrWorld::XrEyeInput TrackedEye(const XrMath::Rig& rig, const Fast::StereoEyeView& view);

    // W0 item 2: classes only (no rendering change), logged per scene and every 5 s.
    void Trace(const Oot3dPicaVisualFrameViewSample& sample, int scene);
    // After each presentation: the 5 s TRIAEVUM_XR_VIEW line; returns true while the render safety holds
    // (fallback draws in 30 consecutive eye frames).
    bool EndPresentation(bool eyesBuilt);

    const XrViewStats& Stats() const { return mStats; }

  private:
    struct Facts {
        XrWorld::XrDrawFacts Draw;
        XrMath::Rows Rows{};
        const std::array<float, 16>* WorldToView = nullptr;
        double OriginDistance = -1.0;
    };
    Facts ExtractFacts(const Oot3dPicaVisualDrawView& view, const Oot3dPicaVisualFrameViewSample& sample,
                       const XrFrameCamera& camera) const;

    bool mWorld = false;
    bool mTrace = false;
    XrPreviewConfig mPreview;
    XrWorld::XrSkyScopes mSkyScopes;
    double mScale = XrMath::kDefaultScale;
    std::array<Oot3dPicaVisualFrameViewSample, 2> mEyeScratch;
    std::array<XrWorld::XrChainCache, 2> mChains;
    XrViewStats mStats;
    bool mFallbackThisFrame = false;
    uint64_t mConsecutiveFallbackFrames = 0;
    std::chrono::steady_clock::time_point mNextLog = std::chrono::steady_clock::now();

    // Trace state.
    struct AtmosphereScope {
        uint64_t Draws = 0, Perspective = 0, FullViewport = 0, OriginSamples = 0;
        double OriginMin = 1e30, OriginMax = 0.0;
    };
    std::map<uint32_t, AtmosphereScope> mTraceScopes;
    std::array<uint64_t, XrWorld::kXrDrawClassCount> mTraceClasses{};
    uint64_t mTraceSamples = 0;
    int mTraceScene = -2;
    std::chrono::steady_clock::time_point mTraceNextLog = std::chrono::steady_clock::now();
    XrFrameCamera mTraceCamera;
    void FlushTrace(const char* reason);
};

}  // namespace Oot3dNativeGame
