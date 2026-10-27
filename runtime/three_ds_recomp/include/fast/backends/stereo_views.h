// In-world VR mode (quest-research/inworld-spec.md §2.4, §3.1): the eye poses and tangents that travel
// with a stereo publication, from the native game runtime (which predicts and renders them) through the
// renderer (SetXrStereoViews) to the OpenXR projection layer (PublishStereo). Plain data, no OpenXR or
// Vulkan types; included by gfx_rendering_api.h, quest_xr.h and the native game runtime.
#pragma once

#include <cstdint>

namespace Fast {

// Renderer views of an immersive frame (GfxRenderingAPI::SelectXrView).
inline constexpr uint32_t kXrViewPanel = 0;     // Q3 screen / cinema picture and (W4) the HUD
inline constexpr uint32_t kXrViewEyeLeft = 1;
inline constexpr uint32_t kXrViewEyeRight = 2;
inline constexpr uint32_t kXrViewCount = 3;

struct StereoEyeView {
    float Orientation[4] = {0.0F, 0.0F, 0.0F, 1.0F};  // x, y, z, w in the world space (STAGE)
    float Position[3] = {0.0F, 0.0F, 0.0F};           // metres
    float TanLeft = -1.0F, TanRight = 1.0F, TanUp = 1.0F, TanDown = -1.0F;
};

struct StereoViews {
    StereoEyeView Eye[2];
    int64_t DisplayTimeNs = 0;  // the predicted display time the poses are for
    uint64_t PaceTick = 0;      // p_N: the display tick the renderer waited for
    uint64_t TargetTick = 0;    // p_N + the shown latency: the tick the poses aim at
    uint32_t RigEpoch = 0;      // rig placements so far: a blink fades in once a pair of its epoch shows
    bool Valid = false;
};

}  // namespace Fast
