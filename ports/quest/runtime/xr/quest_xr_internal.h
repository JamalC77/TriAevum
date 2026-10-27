// Private to quest_xr_*.cpp: OpenXR headers, the process-wide XR state and the functions the three
// units share. Shared code includes only quest_xr.h.
#pragma once

#define TRIAEVUM_QUEST_XR_NO_WAIT_IDLE_REDIRECT 1
#include "quest_xr.h"
#include "quest_xr_log.h"

#include <jni.h>
#include <vulkan/vulkan.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace TriAevumQuestXr {

struct ScreenPose {
    XrPosef Pose{{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, -2.5F}};
    XrExtent2Df Size{3.0F, 1.6875F};
};

struct State {
    // Instance and system (game thread at bring-up, read-only afterwards).
    XrInstance Instance = XR_NULL_HANDLE;
    XrSystemId System = XR_NULL_SYSTEM_ID;
    bool HasRefresh = false, HasPerf = false, HasThread = false, HasTouchPlus = false, HasTouchPro = false;
    PFN_xrGetVulkanGraphicsRequirements2KHR GetVulkanRequirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR CreateVulkanInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR GetVulkanDevice = nullptr;
    PFN_xrCreateVulkanDeviceKHR CreateVulkanDevice = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB EnumerateRefreshRates = nullptr;
    PFN_xrRequestDisplayRefreshRateFB RequestRefreshRate = nullptr;
    PFN_xrGetDisplayRefreshRateFB GetRefreshRate = nullptr;
    PFN_xrPerfSettingsSetPerformanceLevelEXT SetPerformanceLevel = nullptr;
    PFN_xrSetAndroidApplicationThreadKHR SetApplicationThread = nullptr;
    bool HasLocalFloor = false;  // XR_EXT_local_floor
    XrVersion VulkanMin = 0, VulkanMax = 0;
    JavaVM* Vm = nullptr;
    jobject Activity = nullptr;

    // Session (created on the game thread, used by the XR thread, destroyed by StopSession).
    XrSession Session = XR_NULL_HANDLE;
    XrSpace LocalSpace = XR_NULL_HANDLE;
    XrSpace ViewSpace = XR_NULL_HANDLE;
    // In-world: STAGE (bounds >= 1 x 1 m), else LOCAL_FLOOR, else LOCAL (inworld-spec §3.2).
    XrSpace WorldSpace = XR_NULL_HANDLE;
    XrReferenceSpaceType WorldSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    VkInstance VulkanInstance = VK_NULL_HANDLE;
    VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    uint32_t QueueFamily = 0;
    uint32_t QueueIndex = 1;
    VkQueue Queue = VK_NULL_HANDLE;

    std::atomic<int> SessionState{XR_SESSION_STATE_UNKNOWN};
    std::atomic<bool> Running{false};
    std::atomic<bool> Focused{false};
    std::atomic<bool> Exiting{false};
    std::atomic<bool> Stop{false};
    std::atomic<bool> ShutdownRequested{false};
    std::atomic<bool> SessionStopped{false};  // StopSession ran: the sink is detached
    std::atomic<float> RefreshHz{0.0F};
    std::thread Thread;
    int MainTid = 0;
};

State& G();
bool EnvFlag(const char* name, bool fallback = false);
double EnvDouble(const char* name, double fallback);
double NowSeconds();
const char* SessionStateName(int state);
template <typename T>
T XrProc(XrInstance instance, const char* name) {
    PFN_xrVoidFunction function = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &function))) return nullptr;
    return reinterpret_cast<T>(function);
}

// The XR queue lock: xrBeginFrame/xrEndFrame, xrAcquire/ReleaseSwapchainImage and queue-1 submits
// (quest_xr_vulkan.cpp). Holds over 5 ms are logged.
class QueueLock {
  public:
    explicit QueueLock(const char* holder);
    ~QueueLock();
    QueueLock(const QueueLock&) = delete;
    QueueLock& operator=(const QueueLock&) = delete;

  private:
    const char* mHolder;
    std::chrono::steady_clock::time_point mStart;
};

// quest_xr_vulkan.cpp
void CreateQuadSwapchain();        // StartSession, right after the session
void CreateCopyResources();        // StartSession: command pool, timelines, queue
XrSwapchain QuadSwapchain();
VkExtent2D QuadExtent();
bool QuadReleased();
// XR thread, each display frame with shouldRender; `tick` = this display frame's tick (shown latency).
void CopyLatest(uint64_t tick);
void DestroyQuadSwapchain();       // StopSession (all swapchains)
// In-world mode (inworld-spec §3.3): the eye swapchain (arraySize 2), the static fade array and, unless
// TRIAEVUM_XR_HUD=0, the HUD swapchain (panel extent) the panel of each stereo pair is copied into.
void CreateWorldSwapchains();      // StartSession, after CreateQuadSwapchain, before CreateCopyResources
bool PrepareFadeSwapchain();       // XR thread: clears the fade levels once; true when usable
XrSwapchain EyeSwapchain();
VkExtent2D EyeSwapchainExtent();
bool EyeReleased();
XrSwapchain FadeSwapchain();
uint32_t FadeLevels();
XrSwapchain HudSwapchain();        // XR_NULL_HANDLE when the HUD is off
VkExtent2D HudSwapchainExtent();
bool HudReleased();                // a HUD image was copied and released (the layer may show it)
bool ShownViews(Fast::StereoViews* views);  // XR thread: the poses of the pair on the eye swapchain
uint32_t LatestPublishedRigEpoch();         // any thread: the highest RigEpoch PublishStereo received
struct SinkStats {
    uint64_t Publications = 0, Copies = 0, StaleDisplayFrames = 0, IncompleteSkips = 0, PublicationsDropped = 0,
             SwapchainWaitTimeouts = 0, StereoPublications = 0, StereoCopies = 0;
    // In-world HUD: stereo publications that carried their panel, HUD images copied with a pair, and
    // pairs whose HUD copy was skipped (its swapchain image was not ready; the last HUD stays).
    uint64_t HudPublications = 0, HudCopies = 0, HudSkips = 0;
    double CopyWaitMsMax = 0.0;
};
SinkStats TakeSinkStats();

// quest_xr_input.cpp
void CreateActions();              // StartSession, before xrAttachSessionActionSets (inside)
void SyncInput(XrTime displayTime);  // XR thread, FOCUSED
void ReleaseInput();               // XR thread, on leaving FOCUSED
void Haptic(bool left, bool right, int milliseconds);
void DestroyActions();
void LogInteractionProfiles();

// quest_xr_runtime.cpp
void RequestRecenter();
void NoteWorldRecenter(const char* reason);  // in-world: the chord and space changes
LayerMode RequestedLayerMode();              // the mode the runtime asked for last (input routing)
// Ends the run: Exiting/Stop, SDL_QUIT to the game loop, the 15 s quit watchdog (any thread).
void BeginExit(const char* reason);
// The first game image reached the quad: the immersive start worked (removes the start marker).
void NoteFirstImage();

}  // namespace TriAevumQuestXr
