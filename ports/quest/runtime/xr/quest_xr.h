// TriAevum Quest immersive mode: the only XR header shared code includes. No OpenXR or ImGui
// headers here; every declaration exists only in builds with TRIAEVUM_QUEST_OPENXR (the Quest
// runtime, ports/quest/runtime/CMakeLists.txt). At run time XR is selected by TRIAEVUM_XR=1,
// which only TriAevumXrActivity sets; the panel activity runs the unchanged renderer paths.
#pragma once

#if defined(TRIAEVUM_QUEST_OPENXR)

#include <vulkan/vulkan.h>

#include <cstdint>

#include "fast/backends/stereo_views.h"

struct ImGuiIO;

namespace TriAevumQuestXr {

// TRIAEVUM_XR=1 (read once). Valid before any XR call.
bool Requested();
// The private render-target extent (TRIAEVUM_XR_PANEL_WIDTH/HEIGHT, default 1280x720).
VkExtent2D PanelExtent();

// ---- Bring-up, on the game (SDL main) thread, from GfxRenderingAPIVulkan::Init. Failures throw
// std::runtime_error after writing <ext>/logs/xr-start-failed.txt (the launcher then asks).
void CreateInstance();  // loader, XrInstance, system, Vulkan graphics requirements
// Throws only when the application version is below the runtime's minimum (max is advisory).
void CheckVulkanApiVersion(uint32_t applicationApiVersion);
VkInstance CreateVulkanInstance(const VkInstanceCreateInfo& createInfo);  // xrCreateVulkanInstanceKHR
VkPhysicalDevice VulkanPhysicalDevice(VkInstance instance);              // xrGetVulkanGraphicsDevice2KHR
// xrCreateVulkanDeviceKHR. Returns false (and logs TRIAEVUM_XR_DEVICE with both results) when the
// XrResult or the VkResult reports a failure, so the caller can retry another feature chain.
bool TryCreateVulkanDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo& createInfo,
                           const char* chainLabel, VkDevice* device);
void StartSession(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                  uint32_t queueFamily, uint32_t xrQueueIndex);
VkFormat TargetFormat();  // UNORM twin of the quad swapchain's sRGB format
// Records the failure marker and logs TRIAEVUM_XR_ERROR (also called by the backend's own checks).
void NoteStartFailure(const char* reason);

// ---- Renderer sink, on whichever thread runs api.StartFrame/EndFrame/FinishRender.
bool RendererMayRun();  // session FOCUSED (or TRIAEVUM_XR_RUN_UNFOCUSED=1), running, not exiting
void AttachTargets(const VkImage* images, uint32_t count, VkExtent2D extent);
void DetachTargets();             // waits for in-flight copies; a no-op after StopSession
// Drops an unshown publication of `image`, waits until no copy reads it and returns the copy
// timeline value the renderer's next submit must wait for (0: none).
uint64_t WaitTargetFree(uint32_t image);
VkSemaphore CopyTimeline();
VkSemaphore RenderTimeline();
void Publish(uint32_t image, uint64_t renderValue);
VkResult DeviceWaitIdleLocked(VkDevice device);  // every queue externally synchronized

enum class ThreadRole { ApplicationMain, RendererWorker };
void RegisterThread(ThreadRole role, int tid);  // applied once the session exists

// ---- Shutdown (GfxRenderingAPIVulkan::Shutdown): StopSession first, inside `if (mDevice)`, before
// the renderer's own device wait; DestroyVulkanObjects just before vkDestroyDevice.
void StopSession();
void DestroyVulkanObjects();

// ---- Input and game-loop notes, on the game thread.
struct PadSnapshot {
    uint32_t Buttons = 0;  // bit i = SDL_GameControllerButton i
    int16_t LeftX = 0, LeftY = 0, RightX = 0, RightY = 0;  // SDL range, Y up-positive
    int16_t TriggerLeft = 0, TriggerRight = 0;             // 0..32767
};
bool TakePad(PadSnapshot* pad);  // false unless FOCUSED; ORs presses latched since the last take
void FeedImGui(ImGuiIO& io);     // after ImGui_ImplSDL2_NewFrame
bool NoteGuestPaused(bool paused);  // true on a pause/resume edge (logs it)
void NoteGuestRefresh();            // counts guest refreshes that ran while paused (must stay 0)
void NotePacing(uint32_t targetHz, const char* frameRateMode);  // logged once
// One line to logcat (tag TriAevumXR) and stderr (logs/native-stderr.log), for the runtime's XR lines.
void LogLine(const char* line);

// ---- In-world mode (quest-research/inworld-spec.md §3.1). Opt-in: TRIAEVUM_XR_VIEW=world in
// <ext>/triaevum-env.txt (or TRIAEVUM_XR_WORLD=1); the default stays the Q3 screen. Valid before init.
bool WorldRequested();
// The eye image extent (TRIAEVUM_XR_EYE_WIDTH/HEIGHT, default 1024x1104, §2.6). Valid before init.
VkExtent2D EyeExtent();
// Renderer sink, in-world: three private targets per frame slot (images[slot * 3 + view], views
// Fast::kXrView*), extents {panel, eye, eye}. WaitTargetFree takes the image index.
void AttachViewTargets(const VkImage* images, uint32_t slots, const VkExtent2D extents[3]);
// Both eyes of `slot` with the poses they were rendered for (the projection layer shows exactly these).
// `panelValid`: the slot's panel holds this frame's HUD over transparent (premultiplied alpha); it is
// copied with the pair into the HUD swapchain and shown as a quad over the world (W4, minimal).
// `frameCopyToken`: RecordFrameCopy put the pair (and the HUD) into swapchain images inside this frame's
// own submission; the XR thread only releases them once the render timeline reaches `renderValue`.
void PublishStereo(uint32_t slot, uint64_t renderValue, const Fast::StereoViews& views, bool panelValid,
                   uint64_t frameCopyToken = 0);
// The in-frame copy (TRIAEVUM_XR_FRAME_COPY, default on; 0 = the XR queue copies each pair after the frame,
// as before). Renderer, at the end of a world frame that drew a pair, before its command buffer ends: when
// the XR thread holds an acquired and waited eye image, records the copies of slot `slot`'s eyes (and, with
// `hud`, its panel into a held HUD image) into `command` and returns the token to publish with (0: none
// held, the pair goes the XR queue's way). The targets are in PRESENT_SRC before and after.
uint64_t RecordFrameCopy(VkCommandBuffer command, uint32_t slot, bool hud);
// A recorded frame copy whose frame published no pair (never expected): its images go with the next pair.
void AbandonFrameCopy(uint64_t token, uint64_t renderValue);
bool FrameCopyEnabled();  // valid before init
// The in-world HUD quad (TRIAEVUM_XR_HUD, default on). Valid before init; any thread.
bool HudEnabled();
// The panel of `slot` (cinema picture on the quad).
void PublishPanel(uint32_t slot, uint64_t renderValue);
// ---- In-world performance (TRIAEVUM_XR_PERF, every 5 s on the XR thread; TRIAEVUM_XR_PERF=0 turns it off).
// Durations in milliseconds, any thread, lock-free; each phase reports mean/p95/max per 5 s window.
enum class PerfPhase : uint8_t {
    GuestFrame,       // guest thread: one host frame (RunFrame to RunFrame)
    GuestUpdate,      // one guest refresh (RunGuestRefresh)
    GuestStreamWait,  // the guest blocked on the render worker (frame backpressure, synchronous commands)
    GuestCpu,         // the guest thread's CPU time per host frame
    WorkerFrame,      // render worker: end of frame to end of frame
    WorkerStart,      // StartFrame
    WorkerFence,      //   its frame-slot fence wait
    WorkerTargetFree, //   its wait for the XR queue's last copy out of the slot's targets
    WorkerNs1,        // the canonical namespace-1 replay of a new frame (superseded frames included)
    WorkerEyeLeft,    // eye L: built and replayed
    WorkerEyeRight,
    WorkerUi,         // the TopScreen HUD / menu passes (on the panel)
    WorkerTickWait,   // the end-of-frame wait for the pace tick (idle by design)
    WorkerSubmit,     // EndFrame: the in-frame copy and the frame's vkQueueSubmit
    WorkerCpu,        // the render worker's CPU time per frame
    WorkerIdle,       // the render worker waiting for guest commands, per frame
    XrWaitFrame,      // XR thread: xrWaitFrame
    XrBeginFrame,     // xrBeginFrame (under the queue lock)
    XrEndFrame,       // xrEndFrame (under the queue lock)
    XrAcquire,        // xrAcquireSwapchainImage
    XrWaitImage,      // xrWaitSwapchainImage
    XrRelease,        // xrReleaseSwapchainImage
    XrSubmit,         // the XR queue's vkQueueSubmit (copies, release syncs)
    XrCpu,            // the XR thread's CPU time per display frame
    LockHoldXr,       // the XR queue lock held by the XR thread
    LockHoldRenderer, // ... by the renderer (its device waits)
    LockWaitXr,       // waiting for the lock: the XR thread
    LockWaitRenderer, //   the renderer
    PairShown,        // a pair from its publication (just after the frame's submit) to its release
    Count
};
bool PerfEnabled();  // in-world and TRIAEVUM_XR_PERF != 0; valid before init
void PerfNote(PerfPhase phase, double milliseconds);
double ThreadCpuMs();  // the calling thread's CPU time (CLOCK_THREAD_CPUTIME_ID)

// Median display ticks from a pair's PaceTick to its first display (default 2 until measured).
uint32_t ShownLatencyTicks();
struct PairStats {
    uint64_t Pairs = 0;      // stereo pairs shown since the last take
    uint64_t LatePairs = 0;  // first shown after their TargetTick
    uint32_t LatencyTicks = 2;
};
PairStats TakePairStats();

// The display clock (§3.4): published by the XR thread after each xrWaitFrame. Tick counts display
// frames since the session began; DisplayTimeNs is that frame's predicted display time.
struct TickInfo {
    uint64_t Tick = 0;
    int64_t DisplayTimeNs = 0;
    int64_t PeriodNs = 0;
    bool Valid = false;  // the session runs and frames are being waited
};
TickInfo LatestTick();  // any thread
// Render worker: until the display tick `tick` began (true), or timeout / not running (false).
bool WaitDisplayTick(uint64_t tick, uint32_t timeoutMs);
// Eye poses and tangents at a display time in the world space (xrLocateViews); any thread.
bool PredictEyes(int64_t displayTimeNs, Fast::StereoViews* out);
struct HeadPose {
    float Orientation[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    float Position[3] = {0.0F, 0.0F, 0.0F};
    float LinearVelocity[3] = {0.0F, 0.0F, 0.0F};
    bool Valid = false;
};
bool LocateHead(int64_t displayTimeNs, HeadPose* out);  // VIEW in the world space, with velocity
// The world space's play area: STAGE bounds when the world space is STAGE; FloorBelowOriginM is the
// floor's depth under the space origin (LOCAL: 1.65 m, the floor = head height at recenter - 1.65).
struct PlayArea {
    float HalfX = 0.0F, HalfZ = 0.0F;
    bool Bounded = false;
    float FloorBelowOriginM = 0.0F;
    const char* Space = "none";
};
PlayArea WorldPlayArea();
// World recenters since the last take: the L3+R3 chord (in world mode it no longer moves the screen)
// and reference-space changes of the world space or LOCAL.
uint32_t TakeWorldRecenter();
// What the compositor shows (§3.3): World = the projection layer of the latest pair, Cinema = the Q3
// quad (placed again on each entry), Black = nothing. Changes fade through black (fadeMs each way);
// entering World waits (300 ms at most) for a pair of minRigEpoch or later before fading in.
enum class LayerMode : uint8_t { World, Cinema, Black };
void RequestLayerMode(LayerMode mode, uint32_t fadeMs, uint32_t minRigEpoch = 0);
// A rig blink (§5.3): black at once, then a fade-in once a pair of `rigEpoch` is shown.
void RequestBlink(uint32_t rigEpoch, uint32_t fadeMs);
const char* LayerModeName(LayerMode mode);
// Haptic pulse: hand 0 left, 1 right, 2 both.
void Pulse(int hand, uint32_t milliseconds);
// W2 (§3.1, §4.4). Head inside geometry: 0..1 of black over the world while it shows (combined with the
// transition fade by max). Any thread, applied on the next display frame.
void SetWallFade(float alpha);
// W2's right-stick job (§5.5, §7), set by the guest at window setup: armed in mode R and mode L, off for
// the TRIAEVUM_XR_CAMERA=native A/B baseline (W1's right stick: it moves Link while the left stick is
// idle). Armed, the right stick moves Link only while the left controller is inactive
// (RightStickMove::WhenLeftInactive) and takes flicks only while it is active. Any thread.
void SetWorldFlicks(bool armed);
// Right stick X flicks (> 0.7, re-armed below 0.3) while the world shows and flicks are taken, since the
// last take: + right, - left (snap turns, §5.5). Its Y flicks press the D-pad (up / down) for the game.
int32_t TakeSnapTurns();
// The right stick's X while flicks are taken (the smooth-turn option, TRIAEVUM_XR_TURN=smooth); 0 else.
float WorldTurnAxis();

}  // namespace TriAevumQuestXr

// vkDeviceWaitIdle needs every queue of the device externally synchronized; the XR thread and the
// runtime use the binding queue (queue 1). Renderer call sites go through the XR queue lock.
#if !defined(TRIAEVUM_QUEST_XR_NO_WAIT_IDLE_REDIRECT)
#define vkDeviceWaitIdle(device) ::TriAevumQuestXr::DeviceWaitIdleLocked(device)
#endif

#endif  // TRIAEVUM_QUEST_OPENXR
