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
    XrVersion VulkanMin = 0, VulkanMax = 0;
    JavaVM* Vm = nullptr;
    jobject Activity = nullptr;

    // Session (created on the game thread, used by the XR thread, destroyed by StopSession).
    XrSession Session = XR_NULL_HANDLE;
    XrSpace LocalSpace = XR_NULL_HANDLE;
    XrSpace ViewSpace = XR_NULL_HANDLE;
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
void CopyLatest();                 // XR thread, each display frame with shouldRender
void DestroyQuadSwapchain();       // StopSession
struct SinkStats {
    uint64_t Publications = 0, Copies = 0, StaleDisplayFrames = 0, IncompleteSkips = 0, PublicationsDropped = 0,
             SwapchainWaitTimeouts = 0;
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

}  // namespace TriAevumQuestXr
