// TriAevum Quest immersive mode: OpenXR instance and session, the XR frame thread (events, state
// machine, quad layer, screen placement), performance/refresh/thread hints, pause notes and exit.
// q3-spec §4.2, §4.6, §6. In-world: the layer machine, the projection layer and the HUD quad (W4, minimal).
#include "quest_xr_internal.h"
#include "quest_xr_input_logic.h"

#include <SDL2/SDL.h>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace TriAevumQuestXr {

namespace Log {
void Write(int priority, const char* format, ...) {
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof buffer, format, arguments);
    va_end(arguments);
    __android_log_write(priority, "TriAevumXR", buffer);
    std::fprintf(stderr, "%s\n", buffer);
}
}  // namespace Log

namespace {

constexpr double kStatsPeriodSeconds = 5.0;

std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* format, ...) {
    char buffer[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof buffer, format, arguments);
    va_end(arguments);
    return buffer;
}

std::string ExternalPath(const char* relative) {
    const char* root = SDL_AndroidGetExternalStoragePath();
    return std::string(root != nullptr ? root : ".") + "/" + relative;
}

[[noreturn]] void Fail(const std::string& reason) {
    NoteStartFailure(reason.c_str());
    throw std::runtime_error("OpenXR: " + reason);
}

void CheckXr(XrResult result, const char* call) {
    if (XR_FAILED(result)) Fail(Format("%s failed: XrResult %d", call, static_cast<int>(result)));
}

// ---- thread hints (XR_KHR_android_thread_settings), applied on the XR thread once it runs
std::mutex gThreadMutex;
std::vector<std::pair<XrAndroidThreadTypeKHR, int>> gPendingThreads;

const char* ThreadTypeName(XrAndroidThreadTypeKHR type) {
    switch (type) {
    case XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR: return "application_main";
    case XR_ANDROID_THREAD_TYPE_APPLICATION_WORKER_KHR: return "application_worker";
    case XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR: return "renderer_main";
    case XR_ANDROID_THREAD_TYPE_RENDERER_WORKER_KHR: return "renderer_worker";
    default: return "?";
    }
}

void ApplyThreadHint(XrAndroidThreadTypeKHR type, int tid) {
    auto& g = G();
    if (g.SetApplicationThread == nullptr || g.Session == XR_NULL_HANDLE) {
        XRLOG("TRIAEVUM_XR_THREAD role=%s tid=%d result=unsupported", ThreadTypeName(type), tid);
        return;
    }
    const XrResult result = g.SetApplicationThread(g.Session, type, static_cast<uint32_t>(tid));
    XRLOG("TRIAEVUM_XR_THREAD role=%s tid=%d result=%d", ThreadTypeName(type), tid, static_cast<int>(result));
}

void ApplyPendingThreadHints() {
    std::vector<std::pair<XrAndroidThreadTypeKHR, int>> pending;
    {
        std::lock_guard lock(gThreadMutex);
        pending.swap(gPendingThreads);
    }
    for (const auto& [type, tid] : pending) ApplyThreadHint(type, tid);
}

// ---- pause bookkeeping (game thread writes, XR thread reads for the stats)
std::atomic<bool> gGuestPaused{false};
std::atomic<uint64_t> gGuestRefreshesWhilePaused{0};
std::atomic<bool> gRecenterRequested{false};

// ---- in-world state (inworld-spec §3)
std::atomic<uint32_t> gWorldRecenters{0};
// Written at StartSession and when a STAGE reference-space change redraws the boundary (XR thread);
// read by the guest thread at each placement.
std::mutex gPlayAreaMutex;
PlayArea gPlayArea;

// A reference-space change of the world space or LOCAL (§3.2), applied once a display frame reaches
// its changeTime (XR thread only).
struct PendingSpaceChange {
    bool Pending = false;
    XrReferenceSpaceType Type = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrTime ChangeTime = 0;
};
PendingSpaceChange gWorldSpaceChange;

// The display clock, published after each xrWaitFrame (§3.4 G3): the tick counts display periods
// (from the predicted display times), so a late wait that skips a period advances it by two.
std::mutex gTickMutex;
std::condition_variable gTickChanged;
TickInfo gTick;
Logic::DisplayTickClock gTickClock;  // under gTickMutex (written by the XR thread only)

void PublishTick(bool valid, int64_t displayTime, int64_t period) {
    {
        std::lock_guard lock(gTickMutex);
        if (valid) {
            gTick.Tick = gTickClock.Advance(displayTime, period);
            gTick.DisplayTimeNs = displayTime;
            gTick.PeriodNs = period;
        } else {
            gTickClock.Pause();
        }
        gTick.Valid = valid;
    }
    gTickChanged.notify_all();
}

uint64_t CurrentTick() {
    std::lock_guard lock(gTickMutex);
    return gTickClock.Tick;
}

uint64_t TakeSkippedTicks() {
    std::lock_guard lock(gTickMutex);
    return std::exchange(gTickClock.Skipped, uint64_t{0});
}

// Layer modes and fades (§3.3): requests from the runtime's threads, the machine on the XR thread.
struct LayerRequest {
    bool ModePending = false;
    LayerMode Mode = LayerMode::Cinema;
    uint32_t FadeMs = 150;
    uint32_t MinRigEpoch = 0;
    bool BlinkPending = false;
    uint32_t BlinkEpoch = 0;
    uint32_t BlinkMs = 120;
};
std::mutex gLayerMutex;
LayerRequest gLayerRequest;
std::atomic<int> gRequestedLayer{static_cast<int>(LayerMode::Cinema)};

struct LayerMachine {
    enum class Phase : uint8_t { Idle, Out, Hold, In };
    LayerMode Shown = LayerMode::Cinema;
    LayerMode Target = LayerMode::Cinema;
    Phase Step = Phase::Idle;
    double Alpha = 0.0;       // the fade quad's opacity
    double FadeSeconds = 0.15;
    uint32_t HoldEpoch = 0;   // World fades in once a pair of this rig epoch is shown
    double HoldUntil = 0.0;
};
LayerMachine gLayers;  // XR thread only

// ---- in-world HUD (W4, minimal): a lazily body-locked quad in LOCAL over the projection layer, showing
// the panel of the latest pair (the TopScreen HUD and ImGui over transparent, premultiplied alpha).
struct HudLayerConfig {
    Logic::HudFollowConfig Follow;
    float WidthM = 1.05F;
};
const HudLayerConfig& HudConfig() {
    static const HudLayerConfig config = [] {
        constexpr double kToRadians = 3.14159265358979323846 / 180.0;
        HudLayerConfig c;
        // Defaults follow the placement distance (the session clamps TRIAEVUM_XR_PLACE_M the same way):
        // the HUD a little nearer than Link, ~39 deg wide, and a dead zone that keeps its edge within
        // 40 deg of the gaze. Each knob overrides; the follow default is recomputed from the size in use.
        const float place = static_cast<float>(std::clamp(EnvDouble("TRIAEVUM_XR_PLACE_M", 1.8), 0.8, 4.0));
        c.Follow.DistanceM = static_cast<float>(
            std::clamp(EnvDouble("TRIAEVUM_XR_HUD_DISTANCE_M", Logic::DefaultHudDistanceM(place)), 0.5, 10.0));
        c.WidthM = static_cast<float>(
            std::clamp(EnvDouble("TRIAEVUM_XR_HUD_WIDTH_M", Logic::DefaultHudWidthM(c.Follow.DistanceM)), 0.3, 8.0));
        c.Follow.PitchRadians =
            static_cast<float>(std::clamp(EnvDouble("TRIAEVUM_XR_HUD_PITCH_DEG", -8.0), -45.0, 30.0) * kToRadians);
        const double followDefaultDeg =
            Logic::DefaultHudDeadzoneRadians(c.WidthM, c.Follow.DistanceM) / kToRadians;
        c.Follow.DeadzoneRadians = static_cast<float>(
            std::clamp(EnvDouble("TRIAEVUM_XR_HUD_FOLLOW_DEG", followDefaultDeg), 0.0, 90.0) * kToRadians);
        const VkExtent2D panel = PanelExtent();
        XRLOG("TRIAEVUM_XR_HUD config width_m=%.2f height_m=%.3f distance_m=%.2f pitch_deg=%.1f follow_deg=%.1f "
              "edge_deg=%.1f place_m=%.2f space=LOCAL blend=premultiplied",
              c.WidthM, Logic::ScreenHeight(c.WidthM, panel.width, panel.height), c.Follow.DistanceM,
              c.Follow.PitchRadians / kToRadians, c.Follow.DeadzoneRadians / kToRadians,
              Logic::HudHalfAngleRadians(c.WidthM, c.Follow.DistanceM) / kToRadians, place);
        return c;
    }();
    return config;
}
Logic::HudFollow gHudFollow;               // XR thread only
Logic::ScreenPlacement gHudPose;           // XR thread only
bool gHudPoseValid = false;                // XR thread only
std::atomic<bool> gHudReset{true};         // the next HUD frame snaps in front of the head
std::atomic<const char*> gHudResetReason{"enter"};
uint64_t gHudShownFrames = 0;              // XR thread, 5 s window
uint32_t gHudPlacementsLogged = 0;         // XR thread, 5 s window
double gHudLastFollowLog = -10.0;

void ResetHud(const char* reason) {
    gHudResetReason.store(reason);
    gHudReset.store(true);
}

// The HUD quad for this display frame (XR thread). False until a head pose was located once.
bool PlaceHud(XrTime displayTime, double dt, XrCompositionLayerQuad* quad) {
    auto& g = G();
    const HudLayerConfig& config = HudConfig();
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (XR_SUCCEEDED(xrLocateSpace(g.ViewSpace, g.LocalSpace, displayTime, &location)) &&
        (location.locationFlags & kValid) == kValid) {
        const char* reason = "follow";
        if (gHudReset.exchange(false)) {
            gHudFollow.Reset();
            reason = gHudResetReason.load();
        }
        const uint32_t before = gHudFollow.Placements;
        const auto& o = location.pose.orientation;
        const auto& p = location.pose.position;
        gHudPose = gHudFollow.Update({o.x, o.y, o.z, o.w}, {p.x, p.y, p.z}, static_cast<float>(dt), config.Follow);
        gHudPoseValid = true;
        if (gHudFollow.Placements != before) {
            ++gHudPlacementsLogged;
            const double now = NowSeconds();
            if (std::string_view(reason) != "follow" || now - gHudLastFollowLog >= 1.0) {
                if (std::string_view(reason) == "follow") gHudLastFollowLog = now;
                XRLOG("TRIAEVUM_XR_HUD event=place reason=%s yaw_deg=%.1f pos=%.2f,%.2f,%.2f", reason,
                      gHudPose.YawRadians * 57.2957795F, gHudPose.Position.X, gHudPose.Position.Y,
                      gHudPose.Position.Z);
            }
        }
    }
    if (!gHudPoseValid) return false;
    const VkExtent2D extent = HudSwapchainExtent();
    // Premultiplied alpha (no XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT): the panel is cleared to
    // (0, 0, 0, 0) and every overlay blends colour SRC_ALPHA / ONE_MINUS_SRC_ALPHA and alpha ONE /
    // ONE_MINUS_SRC_ALPHA, which over a transparent clear yields colour * alpha with the coverage in alpha.
    quad->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad->space = g.LocalSpace;
    quad->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad->subImage.swapchain = HudSwapchain();
    quad->subImage.imageRect = {{0, 0}, {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height)}};
    quad->subImage.imageArrayIndex = 0;
    quad->pose.orientation = {gHudPose.Orientation.X, gHudPose.Orientation.Y, gHudPose.Orientation.Z,
                              gHudPose.Orientation.W};
    quad->pose.position = {gHudPose.Position.X, gHudPose.Position.Y, gHudPose.Position.Z};
    quad->size = {config.WidthM, Logic::ScreenHeight(config.WidthM, extent.width, extent.height)};
    return true;
}

const char* PhaseName(LayerMachine::Phase phase) {
    switch (phase) {
    case LayerMachine::Phase::Idle: return "idle";
    case LayerMachine::Phase::Out: return "out";
    case LayerMachine::Phase::Hold: return "hold";
    case LayerMachine::Phase::In: return "in";
    }
    return "?";
}

// ---- watchdog: no XR shutdown wait may hang the process (q3-spec §4.6, §6.6). One at a time;
// StopSession disarms it once the XR objects are gone, so the renderer's own shutdown work (the
// preparation join and drain, StorePipelineCache) runs under the longer renderer watchdog only.
constexpr int kWatchdogExitCode = 3;  // a forced exit is visible in the exit status
std::atomic<uint64_t> gWatchdogGeneration{0};
std::atomic<bool> gWatchdogArmed{false};

void ArmWatchdog(const char* stage, int seconds = 15) {
    if (gWatchdogArmed.exchange(true)) return;
    const uint64_t generation = ++gWatchdogGeneration;
    XRLOG("TRIAEVUM_XR_WATCHDOG armed stage=%s seconds=%d", stage, seconds);
    std::thread([stage, seconds, generation] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        if (!gWatchdogArmed.load() || gWatchdogGeneration.load() != generation) return;
        XRERR("TRIAEVUM_XR_EXIT stage=watchdog armed_at=%s seconds=%d exit_code=%d", stage, seconds,
              kWatchdogExitCode);
        std::fflush(nullptr);
        _exit(kWatchdogExitCode);
    }).detach();
}

void DisarmWatchdog() {
    ++gWatchdogGeneration;
    if (gWatchdogArmed.exchange(false)) XRLOG("TRIAEVUM_XR_WATCHDOG disarmed");
}

void PushQuit() {
    SDL_Event quit{};
    quit.type = SDL_QUIT;
    if (SDL_PushEvent(&quit) < 0) XRWARN("TRIAEVUM_XR_EXIT push_quit_failed error=\"%s\"", SDL_GetError());
}

void RemoveStartMarker() {
    std::error_code ignored;
    if (std::filesystem::remove(ExternalPath("logs/xr-start-failed.txt"), ignored))
        XRLOG("TRIAEVUM_XR_START_MARKER removed (the immersive start completed)");
}

// ---- screen (XR thread only)
ScreenPose gScreen;
bool gPlaced = false;
XrTime gPlaceAfter = 0;
const char* gPlaceReason = "first";

void PlaceScreenIfNeeded(XrTime displayTime) {
    auto& g = G();
    if (gRecenterRequested.exchange(false)) {
        gPlaced = false;
        gPlaceAfter = 0;
        gPlaceReason = "recenter";
    }
    if (gPlaced || displayTime < gPlaceAfter) return;
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(g.ViewSpace, g.LocalSpace, displayTime, &location))) return;
    constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((location.locationFlags & kValid) != kValid) return;
    const auto& o = location.pose.orientation;
    const auto& p = location.pose.position;
    const float distance = static_cast<float>(std::clamp(EnvDouble("TRIAEVUM_XR_SCREEN_DISTANCE_M", 2.5), 0.5, 20.0));
    const float width = static_cast<float>(std::clamp(EnvDouble("TRIAEVUM_XR_SCREEN_WIDTH_M", 3.0), 0.3, 30.0));
    const auto placement = Logic::PlaceScreen({o.x, o.y, o.z, o.w}, {p.x, p.y, p.z}, distance);
    const VkExtent2D panel = PanelExtent();
    gScreen.Pose.orientation = {placement.Orientation.X, placement.Orientation.Y, placement.Orientation.Z,
                                placement.Orientation.W};
    gScreen.Pose.position = {placement.Position.X, placement.Position.Y, placement.Position.Z};
    gScreen.Size = {width, Logic::ScreenHeight(width, panel.width, panel.height)};
    gPlaced = true;
    XRLOG("TRIAEVUM_XR_SCREEN yaw_deg=%.1f pos=%.2f,%.2f,%.2f width=%.2f height=%.3f distance=%.2f reason=%s",
          placement.YawRadians * 57.2957795F, placement.Position.X, placement.Position.Y, placement.Position.Z,
          gScreen.Size.width, gScreen.Size.height, distance, gPlaceReason);
    if (std::string_view(gPlaceReason) == "recenter") {
        Haptic(true, true, 60);
        XRLOG("TRIAEVUM_XR_RECENTER yaw_deg=%.1f", placement.YawRadians * 57.2957795F);
    }
}

// ---- session events (XR thread)
void OnSessionRunning() {
    auto& g = G();
    if (g.EnumerateRefreshRates != nullptr && g.RequestRefreshRate != nullptr) {
        uint32_t count = 0;
        g.EnumerateRefreshRates(g.Session, 0, &count, nullptr);
        std::vector<float> rates(count);
        if (count != 0U) g.EnumerateRefreshRates(g.Session, count, &count, rates.data());
        std::string available;
        for (float rate : rates) available += Format("%s%.0f", available.empty() ? "" : "/", rate);
        // In-world: 90 Hz, so the 30 Hz eyes lock 3:1 (inworld-spec §3.4); the Q3 screen keeps 72.
        const float want = static_cast<float>(EnvDouble("TRIAEVUM_XR_REFRESH", WorldRequested() ? 90.0 : 72.0));
        float best = rates.empty() ? 0.0F : rates.front();
        for (float rate : rates)
            if (std::fabs(rate - want) < std::fabs(best - want)) best = rate;
        XrResult result = XR_ERROR_FUNCTION_UNSUPPORTED;
        if (best > 0.0F) result = g.RequestRefreshRate(g.Session, best);
        float current = 0.0F;
        if (g.GetRefreshRate != nullptr) g.GetRefreshRate(g.Session, &current);
        g.RefreshHz = current;
        XRLOG("TRIAEVUM_XR_REFRESH available=%s requested=%.0f result=%d current=%.0f", available.c_str(), best,
              static_cast<int>(result), current);
    }
    if (g.SetPerformanceLevel != nullptr) {
        const XrResult cpu =
            g.SetPerformanceLevel(g.Session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
        const XrResult gpu =
            g.SetPerformanceLevel(g.Session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
        XRLOG("TRIAEVUM_XR_PERF cpu=%d gpu=%d level=sustained_high", static_cast<int>(cpu), static_cast<int>(gpu));
    }
    static bool affinityLogged = false;
    if (!affinityLogged) {
        affinityLogged = true;
        cpu_set_t set;
        CPU_ZERO(&set);
        unsigned long long mask = 0;
        int cpus = 0;
        if (sched_getaffinity(0, sizeof set, &set) == 0) {
            for (int cpu = 0; cpu < 64; ++cpu)
                if (CPU_ISSET(cpu, &set)) {
                    mask |= 1ULL << cpu;
                    ++cpus;
                }
        }
        XRLOG("TRIAEVUM_XR_AFFINITY mask=0x%llX cpus=%d", mask, cpus);
    }
}

void ChangeState(XrSessionState next) {
    auto& g = G();
    const int previous = g.SessionState.exchange(next);
    XRLOG("TRIAEVUM_XR_STATE from=%s to=%s", SessionStateName(previous), SessionStateName(next));
    switch (next) {
    case XR_SESSION_STATE_READY: {
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        const XrResult result = xrBeginSession(g.Session, &begin);
        XRLOG("TRIAEVUM_XR_BEGIN result=%d", static_cast<int>(result));
        if (XR_SUCCEEDED(result)) {
            g.Running = true;
            OnSessionRunning();
        } else {
            // No further READY follows: end the run (summary written) and let the launcher ask.
            NoteStartFailure(Format("xrBeginSession failed: XrResult %d", static_cast<int>(result)).c_str());
            BeginExit("begin_failed");
        }
        break;
    }
    case XR_SESSION_STATE_SYNCHRONIZED:
    case XR_SESSION_STATE_VISIBLE:
        if (g.Focused.exchange(false)) ReleaseInput();
        break;
    case XR_SESSION_STATE_FOCUSED:
        g.Focused = true;
        break;
    case XR_SESSION_STATE_STOPPING: {
        if (g.Focused.exchange(false)) ReleaseInput();
        const XrResult result = xrEndSession(g.Session);
        g.Running = false;
        XRLOG("TRIAEVUM_XR_END result=%d", static_cast<int>(result));
        break;
    }
    case XR_SESSION_STATE_EXITING:
    case XR_SESSION_STATE_LOSS_PENDING:
        BeginExit(SessionStateName(next));
        break;
    default:
        break;
    }
}

void PollEvents() {
    auto& g = G();
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g.Instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            XRWARN("TRIAEVUM_XR_EVENT instance_loss_pending");
            BeginExit("instance_loss_pending");
            break;
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            ChangeState(reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state);
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            const auto& change = reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(event);
            XRLOG("TRIAEVUM_XR_EVENT reference_space_change space=%d change_time=%lld",
                  static_cast<int>(change.referenceSpaceType), static_cast<long long>(change.changeTime));
            if (change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                gPlaced = false;
                gPlaceAfter = change.changeTime;
                gPlaceReason = "space_change";
            }
            // In-world: a user recenter changes LOCAL, not STAGE; either re-places the world (§3.2),
            // once a display frame reaches changeTime (ApplyWorldSpaceChange in the frame loop).
            if (WorldRequested() && (change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ||
                                     change.referenceSpaceType == G().WorldSpaceType)) {
                auto& pending = gWorldSpaceChange;
                // A second change before the first applied: the later time; a change of the world space
                // itself wins over LOCAL (it blinks and, for STAGE, refreshes the bounds).
                if (!pending.Pending || change.referenceSpaceType == G().WorldSpaceType)
                    pending.Type = change.referenceSpaceType;
                pending.ChangeTime =
                    pending.Pending ? std::max(pending.ChangeTime, change.changeTime) : change.changeTime;
                pending.Pending = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            LogInteractionProfiles();
            break;
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            const auto& change = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB&>(event);
            g.RefreshHz = change.toDisplayRefreshRate;
            XRLOG("TRIAEVUM_XR_REFRESH changed from=%.0f to=%.0f", change.fromDisplayRefreshRate,
                  change.toDisplayRefreshRate);
            break;
        }
        case XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT: {
            const auto& perf = reinterpret_cast<const XrEventDataPerfSettingsEXT&>(event);
            XRLOG("TRIAEVUM_XR_PERF notification domain=%d subdomain=%d from=%d to=%d", static_cast<int>(perf.domain),
                  static_cast<int>(perf.subDomain), static_cast<int>(perf.fromLevel), static_cast<int>(perf.toLevel));
            break;
        }
        default:
            break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

struct FrameStats {
    uint64_t DisplayFrames = 0, ShouldRenderFalse = 0;
    double WaitFrameMsMax = 0.0, BeginFrameMsMax = 0.0;
};

void LogStats(FrameStats& frames) {
    const auto& g = G();
    const SinkStats sink = TakeSinkStats();
    XRLOG("TRIAEVUM_XR_STATS state=%s display_frames=%llu should_render_false=%llu copies=%llu new_frames=%llu "
          "stale_display_frames=%llu incomplete_skips=%llu publications_dropped=%llu swapchain_wait_timeouts=%llu "
          "wait_frame_ms_max=%.1f begin_frame_ms_max=%.1f copy_wait_ms_max=%.1f guest_paused=%d "
          "guest_refreshes_while_paused=%llu refresh_hz=%.0f stereo_publications=%llu stereo_copies=%llu "
          "layer=%s target=%s fade=%.2f phase=%s skipped_ticks=%llu",
          SessionStateName(g.SessionState.load()), static_cast<unsigned long long>(frames.DisplayFrames),
          static_cast<unsigned long long>(frames.ShouldRenderFalse), static_cast<unsigned long long>(sink.Copies),
          static_cast<unsigned long long>(sink.Publications),
          static_cast<unsigned long long>(sink.StaleDisplayFrames),
          static_cast<unsigned long long>(sink.IncompleteSkips),
          static_cast<unsigned long long>(sink.PublicationsDropped),
          static_cast<unsigned long long>(sink.SwapchainWaitTimeouts), frames.WaitFrameMsMax, frames.BeginFrameMsMax,
          sink.CopyWaitMsMax, gGuestPaused.load() ? 1 : 0,
          static_cast<unsigned long long>(gGuestRefreshesWhilePaused.load()), g.RefreshHz.load(),
          static_cast<unsigned long long>(sink.StereoPublications), static_cast<unsigned long long>(sink.StereoCopies),
          LayerModeName(gLayers.Shown), LayerModeName(gLayers.Target), gLayers.Alpha, PhaseName(gLayers.Step),
          static_cast<unsigned long long>(TakeSkippedTicks()));
    if (WorldRequested()) {
        // placed = snaps and catch-ups started, published = pairs that carried their HUD, copies = HUD
        // images copied with a pair, skips = pairs whose HUD copy found no ready image, shown_frames =
        // display frames with the HUD layer.
        XRLOG("TRIAEVUM_XR_HUD enabled=%d placed=%u published=%llu copies=%llu skips=%llu shown_frames=%llu "
              "released=%d yaw_deg=%.1f following=%d",
              HudEnabled() ? 1 : 0, gHudPlacementsLogged, static_cast<unsigned long long>(sink.HudPublications),
              static_cast<unsigned long long>(sink.HudCopies), static_cast<unsigned long long>(sink.HudSkips),
              static_cast<unsigned long long>(gHudShownFrames), HudReleased() ? 1 : 0,
              gHudPoseValid ? gHudPose.YawRadians * 57.2957795 : 0.0, gHudFollow.Following ? 1 : 0);
        gHudPlacementsLogged = 0;
        gHudShownFrames = 0;
    }
    frames = {};
}

// The layer machine, once per display frame (§3.3): transitions go out to black over the fade, switch
// the shown mode (the cinema quad is placed again), hold until the new mode has content (a World pair
// of the wanted rig epoch, a placed and copied quad; 300 ms at most), then fade in.
void UpdateLayers(double dt) {
    auto& m = gLayers;
    LayerRequest request;
    {
        std::lock_guard lock(gLayerMutex);
        request = gLayerRequest;
        gLayerRequest.ModePending = false;
        gLayerRequest.BlinkPending = false;
    }
    const double now = NowSeconds();
    if (request.ModePending && request.Mode != m.Target) {
        XRLOG("TRIAEVUM_XR_LAYER request=%s from=%s fade_ms=%u epoch=%u", LayerModeName(request.Mode),
              LayerModeName(m.Shown), request.FadeMs, request.MinRigEpoch);
        m.Target = request.Mode;
        m.FadeSeconds = request.FadeMs / 1000.0;
        m.HoldEpoch = request.MinRigEpoch;
        m.Step = LayerMachine::Phase::Out;
    } else if (request.ModePending) {
        m.HoldEpoch = std::max(m.HoldEpoch, request.MinRigEpoch);
    }
    if (request.BlinkPending && m.Target == LayerMode::World) {
        m.Alpha = 1.0;
        m.HoldEpoch = std::max(m.HoldEpoch, request.BlinkEpoch);
        m.FadeSeconds = request.BlinkMs / 1000.0;
        if (m.Shown == LayerMode::World) {
            m.Step = LayerMachine::Phase::Hold;
            m.HoldUntil = now + 0.3;
        }
    }
    const auto step = [&](double seconds) { return seconds > 0.0 ? dt / seconds : 1.0; };
    switch (m.Step) {
    case LayerMachine::Phase::Out:
        m.Alpha = std::min(1.0, m.Alpha + step(m.FadeSeconds));
        if (m.Alpha >= 1.0) {
            if (m.Shown != m.Target) {
                XRLOG("TRIAEVUM_XR_LAYER shown=%s from=%s", LayerModeName(m.Target), LayerModeName(m.Shown));
            }
            m.Shown = m.Target;
            if (m.Shown == LayerMode::World) ResetHud("enter");  // the HUD starts in front of the head
            if (m.Shown == LayerMode::Cinema) {
                gPlaced = false;  // the screen is placed again on each Cinema entry (§1)
                gPlaceAfter = 0;
                gPlaceReason = "cinema";
            }
            m.Step = m.Shown == LayerMode::Black ? LayerMachine::Phase::Idle : LayerMachine::Phase::Hold;
            m.HoldUntil = now + 0.3;
        }
        break;
    case LayerMachine::Phase::Hold: {
        bool ready = now >= m.HoldUntil;
        if (m.Shown == LayerMode::World) {
            Fast::StereoViews views;
            ready = ready || (EyeReleased() && ShownViews(&views) && views.RigEpoch >= m.HoldEpoch);
        } else if (m.Shown == LayerMode::Cinema) {
            ready = ready || (QuadReleased() && gPlaced);
        }
        if (ready) m.Step = LayerMachine::Phase::In;
        break;
    }
    case LayerMachine::Phase::In:
        m.Alpha = std::max(0.0, m.Alpha - step(m.FadeSeconds));
        if (m.Alpha <= 0.0) m.Step = LayerMachine::Phase::Idle;
        break;
    case LayerMachine::Phase::Idle:
        if (m.Shown == LayerMode::Black) m.Alpha = 1.0;
        break;
    }
}

// A pending reference-space change of the world space or LOCAL (§3.2), once this display frame
// reaches its changeTime: a STAGE change re-reads the bounds; a change that moves the world space goes
// black at once (the pairs on screen were rendered in the old frame) until a pair of the re-placed rig
// is shown; then the world recenter (re-placement) is noted for the guest.
void ApplyWorldSpaceChange(XrTime displayTime) {
    auto& pending = gWorldSpaceChange;
    if (!pending.Pending || (pending.ChangeTime != 0 && displayTime < pending.ChangeTime)) return;
    pending.Pending = false;
    auto& g = G();
    const XrReferenceSpaceType world = g.WorldSpaceType;
    if (pending.Type == XR_REFERENCE_SPACE_TYPE_STAGE && world == XR_REFERENCE_SPACE_TYPE_STAGE) {
        XrExtent2Df bounds{0.0F, 0.0F};
        const XrResult result = xrGetReferenceSpaceBoundsRect(g.Session, XR_REFERENCE_SPACE_TYPE_STAGE, &bounds);
        const bool usable = result == XR_SUCCESS && bounds.width >= 1.0F && bounds.height >= 1.0F;
        {
            std::lock_guard lock(gPlayAreaMutex);
            // Below 1 x 1 m the leash runs unbounded (the space stays STAGE until W2 can switch it).
            gPlayArea.HalfX = usable ? bounds.width * 0.5F : 0.0F;
            gPlayArea.HalfZ = usable ? bounds.height * 0.5F : 0.0F;
            gPlayArea.Bounded = usable;
        }
        XRLOG("TRIAEVUM_XR_WORLD bounds_changed=%.2fx%.2f result=%d bounded=%d", bounds.width, bounds.height,
              static_cast<int>(result), usable ? 1 : 0);
    }
    const bool worldSpaceMoved =
        pending.Type == world ||
        (pending.Type == XR_REFERENCE_SPACE_TYPE_LOCAL &&
         (world == XR_REFERENCE_SPACE_TYPE_LOCAL || world == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT));
    if (worldSpaceMoved) {
        // Black from this display frame on (the layer machine runs below in the same frame); the hold
        // ends with a pair of the rig the guest places next (its epoch is past every published one)
        // or after 300 ms.
        RequestBlink(LatestPublishedRigEpoch() + 1U, 150);
    }
    NoteWorldRecenter(worldSpaceMoved ? "space_change" : "local_change");
}

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void XrThreadMain() {
    auto& g = G();
    pthread_setname_np(pthread_self(), "TriAevumXR");
    ApplyThreadHint(XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, gettid());
    XRLOG("TRIAEVUM_XR_THREAD_START tid=%d", gettid());
    const bool world = WorldRequested();
    FrameStats frames;
    double nextStats = NowSeconds() + kStatsPeriodSeconds;
    bool wasFocused = false;
    bool tickValid = false;
    while (!g.Stop.load()) {
        ApplyPendingThreadHints();
        PollEvents();
        if (g.Stop.load()) break;
        if (NowSeconds() >= nextStats) {
            LogStats(frames);
            nextStats = NowSeconds() + kStatsPeriodSeconds;
        }
        if (!g.Running.load()) {
            if (tickValid) {
                tickValid = false;
                PublishTick(false, 0, 0);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        auto start = std::chrono::steady_clock::now();
        XrResult result = xrWaitFrame(g.Session, &waitInfo, &frameState);
        frames.WaitFrameMsMax = std::max(frames.WaitFrameMsMax, MillisecondsSince(start));
        if (XR_FAILED(result)) {
            XRWARN("TRIAEVUM_XR_ERROR call=xrWaitFrame result=%d", static_cast<int>(result));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        // The display clock (§3.4): tick N begins now; the renderer's tick waits release here.
        tickValid = true;
        PublishTick(true, frameState.predictedDisplayTime, frameState.predictedDisplayPeriod);
        start = std::chrono::steady_clock::now();
        {
            QueueLock lock("xr");
            XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
            result = xrBeginFrame(g.Session, &beginInfo);
        }
        frames.BeginFrameMsMax = std::max(frames.BeginFrameMsMax, MillisecondsSince(start));
        if (XR_FAILED(result)) {
            XRWARN("TRIAEVUM_XR_ERROR call=xrBeginFrame result=%d", static_cast<int>(result));
            continue;
        }
        const bool focused = g.Focused.load();
        if (focused) {
            SyncInput(frameState.predictedDisplayTime);
        } else if (wasFocused) {
            ReleaseInput();
        }
        wasFocused = focused;
        PlaceScreenIfNeeded(frameState.predictedDisplayTime);
        if (world) ApplyWorldSpaceChange(frameState.predictedDisplayTime);
        if (frameState.shouldRender) {
            CopyLatest(CurrentTick());
        } else {
            ++frames.ShouldRenderFalse;
        }
        const double period = frameState.predictedDisplayPeriod > 0 ? frameState.predictedDisplayPeriod * 1e-9 : 1.0 / 90.0;
        if (world) UpdateLayers(period);
        const bool fadeReady = world && PrepareFadeSwapchain();

        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerProjectionView projectionViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                               {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerQuad fade{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad hud{XR_TYPE_COMPOSITION_LAYER_QUAD};
        const XrCompositionLayerBaseHeader* layers[4] = {};
        uint32_t layerCount = 0;
        const auto addQuad = [&] {
            const VkExtent2D extent = QuadExtent();
            quad.layerFlags = 0;  // opaque
            quad.space = g.LocalSpace;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = QuadSwapchain();
            quad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height)}};
            quad.subImage.imageArrayIndex = 0;
            quad.pose = gScreen.Pose;
            quad.size = gScreen.Size;
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        };
        if (!world) {
            if (frameState.shouldRender && QuadReleased() && gPlaced) addQuad();
        } else if (frameState.shouldRender && gLayers.Alpha < 0.999) {
            Fast::StereoViews shown;
            if (gLayers.Shown == LayerMode::World && EyeReleased() && ShownViews(&shown)) {
                // The pose and the image always come from the same publication (§3.3).
                const VkExtent2D extent = EyeSwapchainExtent();
                for (uint32_t eye = 0; eye < 2; ++eye) {
                    auto& view = projectionViews[eye];
                    const auto& source = shown.Eye[eye];
                    view.pose.orientation = {source.Orientation[0], source.Orientation[1], source.Orientation[2],
                                             source.Orientation[3]};
                    view.pose.position = {source.Position[0], source.Position[1], source.Position[2]};
                    view.fov = {std::atan(source.TanLeft), std::atan(source.TanRight), std::atan(source.TanUp),
                                std::atan(source.TanDown)};
                    view.subImage.swapchain = EyeSwapchain();
                    view.subImage.imageRect = {{0, 0},
                                               {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height)}};
                    view.subImage.imageArrayIndex = eye;
                }
                projection.layerFlags = 0;  // opaque
                projection.space = g.WorldSpace;
                projection.viewCount = 2;
                projection.views = projectionViews;
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
                // The HUD over the world (under the fade): hearts, rings and rupees of the shown pair's frame.
                if (HudEnabled() && HudReleased() && PlaceHud(frameState.predictedDisplayTime, period, &hud)) {
                    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hud);
                    ++gHudShownFrames;
                }
            } else if (gLayers.Shown == LayerMode::Cinema && QuadReleased() && gPlaced) {
                addQuad();
            }
            const auto level = static_cast<uint32_t>(std::lround(gLayers.Alpha * (FadeLevels() - 1)));
            if (layerCount != 0U && level != 0U && fadeReady) {
                // Black over the scene, head-locked: 2 m wide at 0.5 m covers the field of view.
                fade.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                fade.space = g.ViewSpace;
                fade.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                fade.subImage.swapchain = FadeSwapchain();
                fade.subImage.imageRect = {{0, 0}, {4, 4}};
                fade.subImage.imageArrayIndex = level;
                fade.pose = {{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, -0.5F}};
                fade.size = {2.0F, 2.0F};
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&fade);
            }
        }
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = layerCount;
        endInfo.layers = layerCount != 0U ? layers : nullptr;
        {
            QueueLock lock("xr");
            result = xrEndFrame(g.Session, &endInfo);
        }
        if (XR_FAILED(result)) XRWARN("TRIAEVUM_XR_ERROR call=xrEndFrame result=%d", static_cast<int>(result));
        ++frames.DisplayFrames;
    }
    PublishTick(false, 0, 0);
    XRLOG("TRIAEVUM_XR_THREAD_EXIT exiting=%d shutdown_requested=%d", g.Exiting.load() ? 1 : 0,
          g.ShutdownRequested.load() ? 1 : 0);
}

}  // namespace

State& G() {
    static State state;
    return state;
}

bool EnvFlag(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    return std::string_view(value) == "1" || std::string_view(value) == "true";
}

double EnvDouble(const char* name, double fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    return end != value && std::isfinite(parsed) ? parsed : fallback;
}

double NowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

const char* SessionStateName(int state) {
    switch (state) {
    case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "?";
    }
}

void RequestRecenter() { gRecenterRequested = true; }

void NoteWorldRecenter(const char* reason) {
    ResetHud("recenter");
    const uint32_t count = ++gWorldRecenters;
    XRLOG("TRIAEVUM_XR_RECENTER world=1 reason=%s pending=%u", reason, count);
}

LayerMode RequestedLayerMode() { return static_cast<LayerMode>(gRequestedLayer.load()); }

void BeginExit(const char* reason) {
    auto& g = G();
    g.Focused = false;
    g.Exiting = true;
    g.Stop = true;
    if (g.ShutdownRequested.load()) return;
    // Quit from the system menu, a lost session or a fatal XR error: the game loop ends through
    // SDL_QUIT, writes its summary, and Shutdown destroys the XR objects (StopSession).
    PushQuit();
    XRLOG("TRIAEVUM_XR_EXIT stage=quit_event state=%s", reason);
    ArmWatchdog("quit_event");
}

void NoteFirstImage() { RemoveStartMarker(); }

void LogLine(const char* line) { XRLOG("%s", line); }

VkExtent2D EyeExtent() {
    static const VkExtent2D extent = [] {
        const auto dimension = [](const char* name, double fallback) {
            return static_cast<uint32_t>(std::clamp(std::lround(EnvDouble(name, fallback)), 256L, 4096L));
        };
        return VkExtent2D{dimension("TRIAEVUM_XR_EYE_WIDTH", 1024.0), dimension("TRIAEVUM_XR_EYE_HEIGHT", 1104.0)};
    }();
    return extent;
}

bool HudEnabled() {
    static const bool enabled = [] {
        const bool on = EnvFlag("TRIAEVUM_XR_HUD", true);
        XRLOG("TRIAEVUM_XR_HUD select enabled=%d", on ? 1 : 0);
        return on;
    }();
    return enabled;
}

bool WorldRequested() {
    static const bool requested = [] {
        const char* view = std::getenv("TRIAEVUM_XR_VIEW");
        const bool world = (view != nullptr && std::string_view(view) == "world") || EnvFlag("TRIAEVUM_XR_WORLD");
        XRLOG("TRIAEVUM_XR_VIEW_SELECT view=%s xr=%d", world ? "world" : "screen", Requested() ? 1 : 0);
        return world;
    }();
    return requested;
}

bool Requested() {
    static const bool requested = [] {
        const bool enabled = EnvFlag("TRIAEVUM_XR");
        const VkExtent2D panel = PanelExtent();
        XRLOG("TRIAEVUM_XR_MODE xr=%d build_openxr=1 panel=%ux%u", enabled ? 1 : 0, panel.width, panel.height);
        return enabled;
    }();
    return requested;
}

VkExtent2D PanelExtent() {
    static const VkExtent2D extent = [] {
        const auto dimension = [](const char* name, double fallback) {
            return static_cast<uint32_t>(std::clamp(std::lround(EnvDouble(name, fallback)), 320L, 4096L));
        };
        return VkExtent2D{dimension("TRIAEVUM_XR_PANEL_WIDTH", 1280.0), dimension("TRIAEVUM_XR_PANEL_HEIGHT", 720.0)};
    }();
    return extent;
}

void NoteStartPending() {
    const std::string path = ExternalPath("logs/xr-start-failed.txt");
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    std::ofstream marker(path, std::ios::trunc);
    marker << "pending: the immersive start did not complete (no game image reached the headset)\n";
}

void NoteStartFailure(const char* reason) {
    XRERR("TRIAEVUM_XR_ERROR stage=start reason=\"%s\"", reason);
    const std::string path = ExternalPath("logs/xr-start-failed.txt");
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    std::ofstream marker(path, std::ios::trunc);
    marker << reason << '\n';
}

void CreateInstance() {
    auto& g = G();
    if (g.Instance != XR_NULL_HANDLE) return;
    // Sentinel: replaced by the reason on a known failure, removed at the first image. A throw, a
    // crash or a hang anywhere before that makes the next launch ask (TriAevumLauncherActivity).
    NoteStartPending();
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (env == nullptr || env->GetJavaVM(&g.Vm) != JNI_OK) Fail("no JavaVM from SDL");
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (activity == nullptr) Fail("no Activity from SDL");
    g.Activity = env->NewGlobalRef(activity);
    env->DeleteLocalRef(activity);

    auto initializeLoader = XrProc<PFN_xrInitializeLoaderKHR>(XR_NULL_HANDLE, "xrInitializeLoaderKHR");
    if (initializeLoader == nullptr) Fail("xrInitializeLoaderKHR is missing");
    XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInfo.applicationVM = g.Vm;
    loaderInfo.applicationContext = g.Activity;
    CheckXr(initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo)),
            "xrInitializeLoaderKHR");

    uint32_t count = 0;
    CheckXr(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties");
    std::vector<XrExtensionProperties> offered(count, {XR_TYPE_EXTENSION_PROPERTIES});
    CheckXr(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, offered.data()),
            "xrEnumerateInstanceExtensionProperties");
    std::string line;
    for (size_t index = 0; index < offered.size(); ++index) {
        line += Format("%s%s", line.empty() ? "" : " ", offered[index].extensionName);
        if ((index + 1) % 8 == 0 || index + 1 == offered.size()) {
            XRLOG("TRIAEVUM_XR_EXTENSIONS %s", line.c_str());
            line.clear();
        }
    }
    const auto has = [&](const char* name) {
        return std::any_of(offered.begin(), offered.end(),
                           [&](const XrExtensionProperties& p) { return std::strcmp(p.extensionName, name) == 0; });
    };
    for (const char* required : {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME})
        if (!has(required)) Fail(Format("the runtime lacks %s", required));
    std::vector<const char*> extensions{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
                                        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME};
    const auto optional = [&](const char* name, bool& flag) {
        flag = has(name);
        if (flag) extensions.push_back(name);
    };
    optional(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, g.HasRefresh);
    optional(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME, g.HasPerf);
    optional(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME, g.HasThread);
    optional("XR_META_touch_controller_plus", g.HasTouchPlus);
    optional("XR_FB_touch_controller_pro", g.HasTouchPro);
    if (WorldRequested()) optional(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME, g.HasLocalFloor);

    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = g.Vm;
    androidInfo.applicationActivity = g.Activity;
    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.next = &androidInfo;
    std::strncpy(createInfo.applicationInfo.applicationName, "TriAevum", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(createInfo.applicationInfo.engineName, "TriAevum", XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.enabledExtensionNames = extensions.data();
    CheckXr(xrCreateInstance(&createInfo, &g.Instance), "xrCreateInstance");

    XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
    CheckXr(xrGetInstanceProperties(g.Instance, &instanceProperties), "xrGetInstanceProperties");
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    CheckXr(xrGetSystem(g.Instance, &systemInfo, &g.System), "xrGetSystem");
    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
    CheckXr(xrGetSystemProperties(g.Instance, g.System, &systemProperties), "xrGetSystemProperties");

    g.GetVulkanRequirements =
        XrProc<PFN_xrGetVulkanGraphicsRequirements2KHR>(g.Instance, "xrGetVulkanGraphicsRequirements2KHR");
    g.CreateVulkanInstance = XrProc<PFN_xrCreateVulkanInstanceKHR>(g.Instance, "xrCreateVulkanInstanceKHR");
    g.GetVulkanDevice = XrProc<PFN_xrGetVulkanGraphicsDevice2KHR>(g.Instance, "xrGetVulkanGraphicsDevice2KHR");
    g.CreateVulkanDevice = XrProc<PFN_xrCreateVulkanDeviceKHR>(g.Instance, "xrCreateVulkanDeviceKHR");
    if (!g.GetVulkanRequirements || !g.CreateVulkanInstance || !g.GetVulkanDevice || !g.CreateVulkanDevice)
        Fail("XR_KHR_vulkan_enable2 functions are missing");
    if (g.HasRefresh) {
        g.EnumerateRefreshRates =
            XrProc<PFN_xrEnumerateDisplayRefreshRatesFB>(g.Instance, "xrEnumerateDisplayRefreshRatesFB");
        g.RequestRefreshRate = XrProc<PFN_xrRequestDisplayRefreshRateFB>(g.Instance, "xrRequestDisplayRefreshRateFB");
        g.GetRefreshRate = XrProc<PFN_xrGetDisplayRefreshRateFB>(g.Instance, "xrGetDisplayRefreshRateFB");
    }
    if (g.HasPerf)
        g.SetPerformanceLevel =
            XrProc<PFN_xrPerfSettingsSetPerformanceLevelEXT>(g.Instance, "xrPerfSettingsSetPerformanceLevelEXT");
    if (g.HasThread)
        g.SetApplicationThread =
            XrProc<PFN_xrSetAndroidApplicationThreadKHR>(g.Instance, "xrSetAndroidApplicationThreadKHR");

    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    CheckXr(g.GetVulkanRequirements(g.Instance, g.System, &requirements), "xrGetVulkanGraphicsRequirements2KHR");
    g.VulkanMin = requirements.minApiVersionSupported;
    g.VulkanMax = requirements.maxApiVersionSupported;
    XRLOG("TRIAEVUM_XR_INSTANCE runtime=\"%s %u.%u.%u\" system=\"%s\" ext_refresh=%d ext_perf=%d ext_thread=%d "
          "ext_touch_plus=%d ext_touch_pro=%d vk_min=%u.%u vk_max=%u.%u",
          instanceProperties.runtimeName, XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
          XR_VERSION_MINOR(instanceProperties.runtimeVersion), XR_VERSION_PATCH(instanceProperties.runtimeVersion),
          systemProperties.systemName, g.HasRefresh, g.HasPerf, g.HasThread, g.HasTouchPlus, g.HasTouchPro,
          static_cast<unsigned>(XR_VERSION_MAJOR(g.VulkanMin)), static_cast<unsigned>(XR_VERSION_MINOR(g.VulkanMin)),
          static_cast<unsigned>(XR_VERSION_MAJOR(g.VulkanMax)), static_cast<unsigned>(XR_VERSION_MINOR(g.VulkanMax)));
}

void CheckVulkanApiVersion(uint32_t applicationApiVersion) {
    const auto& g = G();
    // XrVersion packs major<<48 | minor<<32; Vulkan packs its own way: compare major.minor only.
    const uint32_t application =
        VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(applicationApiVersion), VK_API_VERSION_MINOR(applicationApiVersion), 0);
    const uint32_t minimum = VK_MAKE_API_VERSION(0, static_cast<uint32_t>(XR_VERSION_MAJOR(g.VulkanMin)),
                                                 static_cast<uint32_t>(XR_VERSION_MINOR(g.VulkanMin)), 0);
    const uint32_t maximum = VK_MAKE_API_VERSION(0, static_cast<uint32_t>(XR_VERSION_MAJOR(g.VulkanMax)),
                                                 static_cast<uint32_t>(XR_VERSION_MINOR(g.VulkanMax)), 0);
    if (application < minimum)
        Fail(Format("the runtime needs Vulkan %u.%u or newer; the renderer asks for %u.%u",
                    VK_API_VERSION_MAJOR(minimum), VK_API_VERSION_MINOR(minimum),
                    VK_API_VERSION_MAJOR(application), VK_API_VERSION_MINOR(application)));
    if (maximum != 0U && application > maximum)
        XRWARN("TRIAEVUM_XR_VULKAN_VERSION app=%u.%u above the runtime's tested maximum %u.%u (continuing)",
               VK_API_VERSION_MAJOR(application), VK_API_VERSION_MINOR(application), VK_API_VERSION_MAJOR(maximum),
               VK_API_VERSION_MINOR(maximum));
}

VkInstance CreateVulkanInstance(const VkInstanceCreateInfo& createInfo) {
    auto& g = G();
    XrVulkanInstanceCreateInfoKHR info{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    info.systemId = g.System;
    info.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    info.vulkanCreateInfo = &createInfo;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult vulkanResult = VK_SUCCESS;
    const XrResult result = g.CreateVulkanInstance(g.Instance, &info, &instance, &vulkanResult);
    XRLOG("TRIAEVUM_XR_VULKAN_INSTANCE xr=%d vk=%d extensions=%u", static_cast<int>(result),
          static_cast<int>(vulkanResult), createInfo.enabledExtensionCount);
    if (XR_FAILED(result) || vulkanResult != VK_SUCCESS || instance == VK_NULL_HANDLE)
        Fail(Format("xrCreateVulkanInstanceKHR failed: XrResult %d, VkResult %d", static_cast<int>(result),
                    static_cast<int>(vulkanResult)));
    return instance;
}

VkPhysicalDevice VulkanPhysicalDevice(VkInstance instance) {
    auto& g = G();
    XrVulkanGraphicsDeviceGetInfoKHR info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    info.systemId = g.System;
    info.vulkanInstance = instance;
    VkPhysicalDevice device = VK_NULL_HANDLE;
    CheckXr(g.GetVulkanDevice(g.Instance, &info, &device), "xrGetVulkanGraphicsDevice2KHR");
    if (device == VK_NULL_HANDLE) Fail("xrGetVulkanGraphicsDevice2KHR returned no device");
    return device;
}

bool TryCreateVulkanDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo& createInfo,
                           const char* chainLabel, VkDevice* device) {
    auto& g = G();
    std::string chain;
    for (auto* next = static_cast<const VkBaseInStructure*>(createInfo.pNext); next != nullptr; next = next->pNext)
        chain += Format("%s%d", chain.empty() ? "" : ",", static_cast<int>(next->sType));
    XrVulkanDeviceCreateInfoKHR info{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    info.systemId = g.System;
    info.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    info.vulkanPhysicalDevice = physicalDevice;
    info.vulkanCreateInfo = &createInfo;
    VkResult vulkanResult = VK_SUCCESS;
    *device = VK_NULL_HANDLE;
    const XrResult result = g.CreateVulkanDevice(g.Instance, &info, device, &vulkanResult);
    XRLOG("TRIAEVUM_XR_DEVICE chain=%s xr=%d vk=%d structs=%s extensions=%u", chainLabel, static_cast<int>(result),
          static_cast<int>(vulkanResult), chain.c_str(), createInfo.enabledExtensionCount);
    if (XR_FAILED(result) || vulkanResult != VK_SUCCESS || *device == VK_NULL_HANDLE) {
        *device = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

// In-world (inworld-spec §3.2, G1, G2): STAGE when offered with bounds >= 1 x 1 m, else LOCAL_FLOOR,
// else LOCAL (floor 1.65 m under the origin); the view configuration's recommended size is logged.
void CreateWorldSpace() {
    auto& g = G();
    uint32_t count = 0;
    xrEnumerateReferenceSpaces(g.Session, 0, &count, nullptr);
    std::vector<XrReferenceSpaceType> spaces(count);
    if (count != 0U) xrEnumerateReferenceSpaces(g.Session, count, &count, spaces.data());
    const auto offered = [&](XrReferenceSpaceType type) {
        return std::find(spaces.begin(), spaces.end(), type) != spaces.end();
    };
    std::string list;
    for (auto type : spaces) list += Format("%s%d", list.empty() ? "" : ",", static_cast<int>(type));
    XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    info.poseInReferenceSpace.orientation.w = 1.0F;
    XrExtent2Df bounds{0.0F, 0.0F};
    const bool forceLocalFloor = std::string_view(std::getenv("TRIAEVUM_XR_WORLD_SPACE") != nullptr
                                                      ? std::getenv("TRIAEVUM_XR_WORLD_SPACE") : "") == "local_floor";
    if (offered(XR_REFERENCE_SPACE_TYPE_STAGE) && !forceLocalFloor) {
        info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        XrSpace stage = XR_NULL_HANDLE;
        const XrResult created = xrCreateReferenceSpace(g.Session, &info, &stage);
        const XrResult bounded = XR_SUCCEEDED(created)
            ? xrGetReferenceSpaceBoundsRect(g.Session, XR_REFERENCE_SPACE_TYPE_STAGE, &bounds)
            : created;
        if (XR_SUCCEEDED(created) && bounded == XR_SUCCESS && bounds.width >= 1.0F && bounds.height >= 1.0F) {
            g.WorldSpace = stage;
            g.WorldSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
            std::lock_guard lock(gPlayAreaMutex);
            gPlayArea = {bounds.width * 0.5F, bounds.height * 0.5F, true, 0.0F, "STAGE"};
        } else {
            XRLOG("TRIAEVUM_XR_WORLD stage_rejected create=%d bounds_result=%d bounds=%.2fx%.2f",
                  static_cast<int>(created), static_cast<int>(bounded), bounds.width, bounds.height);
            if (stage != XR_NULL_HANDLE) xrDestroySpace(stage);
        }
    }
    if (g.WorldSpace == XR_NULL_HANDLE && g.HasLocalFloor && offered(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT)) {
        info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
        if (XR_SUCCEEDED(xrCreateReferenceSpace(g.Session, &info, &g.WorldSpace))) {
            g.WorldSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
            std::lock_guard lock(gPlayAreaMutex);
            gPlayArea = {0.0F, 0.0F, false, 0.0F, "LOCAL_FLOOR"};
        }
    }
    if (g.WorldSpace == XR_NULL_HANDLE) {
        info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        CheckXr(xrCreateReferenceSpace(g.Session, &info, &g.WorldSpace), "xrCreateReferenceSpace(world LOCAL)");
        g.WorldSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        std::lock_guard lock(gPlayAreaMutex);
        gPlayArea = {0.0F, 0.0F, false, 1.65F, "LOCAL"};
    }
    // The view configuration (G2): the runtime's recommended eye size beside the chosen one (§2.6 gate).
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(g.Instance, g.System, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount,
                                      nullptr);
    std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (viewCount != 0U)
        xrEnumerateViewConfigurationViews(g.Instance, g.System, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount,
                                          &viewCount, views.data());
    const VkExtent2D eye = EyeExtent();
    const PlayArea area = WorldPlayArea();
    XRLOG("TRIAEVUM_XR_WORLD space=%s bounds=%.2fx%.2f floor_below_origin_m=%.2f offered_spaces=%s eye=%ux%u "
          "recommended=%ux%u max=%ux%u views=%u local_floor_ext=%d",
          area.Space, area.HalfX * 2.0F, area.HalfZ * 2.0F, area.FloorBelowOriginM, list.c_str(),
          eye.width, eye.height, viewCount != 0U ? views[0].recommendedImageRectWidth : 0U,
          viewCount != 0U ? views[0].recommendedImageRectHeight : 0U,
          viewCount != 0U ? views[0].maxImageRectWidth : 0U, viewCount != 0U ? views[0].maxImageRectHeight : 0U,
          viewCount, g.HasLocalFloor ? 1 : 0);
}

void StartSession(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamily,
                  uint32_t xrQueueIndex) {
    auto& g = G();
    g.VulkanInstance = instance;
    g.PhysicalDevice = physicalDevice;
    g.Device = device;
    g.QueueFamily = queueFamily;
    g.QueueIndex = xrQueueIndex;

    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = instance;
    binding.physicalDevice = physicalDevice;
    binding.device = device;
    binding.queueFamilyIndex = queueFamily;
    binding.queueIndex = xrQueueIndex;
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = g.System;
    CheckXr(xrCreateSession(g.Instance, &sessionInfo, &g.Session), "xrCreateSession");

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0F;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    CheckXr(xrCreateReferenceSpace(g.Session, &spaceInfo, &g.LocalSpace), "xrCreateReferenceSpace(LOCAL)");
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    CheckXr(xrCreateReferenceSpace(g.Session, &spaceInfo, &g.ViewSpace), "xrCreateReferenceSpace(VIEW)");

    if (WorldRequested()) CreateWorldSpace();

    vkGetDeviceQueue(device, queueFamily, xrQueueIndex, &g.Queue);
    if (g.Queue == VK_NULL_HANDLE) Fail(Format("no Vulkan queue %u in family %u", xrQueueIndex, queueFamily));
    try {
        CreateQuadSwapchain();
        if (WorldRequested()) CreateWorldSwapchains();
        CreateCopyResources();
        CreateActions();
    } catch (const std::exception& error) {
        Fail(error.what());
    }
    XRLOG("TRIAEVUM_XR_SESSION created queue_family=%u renderer_queue=0 xr_queue=%u", queueFamily, xrQueueIndex);
    g.Thread = std::thread(XrThreadMain);
}

void RegisterThread(ThreadRole role, int tid) {
    auto& g = G();
    if (role == ThreadRole::ApplicationMain) {
        g.MainTid = tid;
    } else if (tid == g.MainTid) {
        return;  // serial presentation: the game thread renders
    }
    std::lock_guard lock(gThreadMutex);
    gPendingThreads.emplace_back(role == ThreadRole::ApplicationMain ? XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR
                                                                     : XR_ANDROID_THREAD_TYPE_RENDERER_WORKER_KHR,
                                 tid);
}

bool RendererMayRun() {
    const auto& g = G();
    // TRIAEVUM_XR_RUN_UNFOCUSED=1 (automated device runs): the renderer and the guest run whatever
    // the session state, headset off included; unshown frames are dropped (WaitTargetFree).
    static const bool runUnfocused = EnvFlag("TRIAEVUM_XR_RUN_UNFOCUSED");
    if (g.Exiting.load() || g.Stop.load() || g.SessionStopped.load()) return false;
    return runUnfocused || (g.Running.load() && g.Focused.load());
}

void StopSession() {
    auto& g = G();
    static std::atomic<bool> entered{false};
    if (!Requested() || entered.exchange(true)) return;
    ArmWatchdog("xr_shutdown");
    XRLOG("TRIAEVUM_XR_EXIT stage=xr_shutdown state=%s running=%d", SessionStateName(g.SessionState.load()),
          g.Running.load() ? 1 : 0);
    g.ShutdownRequested = true;
    if (g.Session != XR_NULL_HANDLE && g.Thread.joinable() && g.Running.load() && !g.Exiting.load()) {
        const XrResult result = xrRequestExitSession(g.Session);
        XRLOG("TRIAEVUM_XR_EXIT stage=request_exit result=%d", static_cast<int>(result));
        const double deadline = NowSeconds() + 2.0;
        while (XR_SUCCEEDED(result) && !g.Exiting.load() && NowSeconds() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Always: with the session not running (headset off) the loop would otherwise never end.
    g.Stop = true;
    if (g.Thread.joinable()) g.Thread.join();
    if (g.Queue != VK_NULL_HANDLE) {
        QueueLock lock("shutdown");
        vkQueueWaitIdle(g.Queue);
    }
    g.SessionStopped = true;
    DestroyQuadSwapchain();
    DestroyActions();
    if (g.ViewSpace != XR_NULL_HANDLE) xrDestroySpace(g.ViewSpace);
    if (g.LocalSpace != XR_NULL_HANDLE) xrDestroySpace(g.LocalSpace);
    if (g.WorldSpace != XR_NULL_HANDLE) xrDestroySpace(g.WorldSpace);
    g.ViewSpace = g.LocalSpace = g.WorldSpace = XR_NULL_HANDLE;
    if (g.Session != XR_NULL_HANDLE) {
        const XrResult result = xrDestroySession(g.Session);
        XRLOG("TRIAEVUM_XR_EXIT stage=session_destroyed result=%d", static_cast<int>(result));
        g.Session = XR_NULL_HANDLE;
    }
    // The XR waits are over. The renderer's shutdown (preparation join and drain, the pipeline
    // cache store) runs as on the panel, bounded only by a separate, generous watchdog.
    DisarmWatchdog();
    ArmWatchdog("renderer_shutdown",
                static_cast<int>(std::clamp(EnvDouble("TRIAEVUM_XR_SHUTDOWN_WATCHDOG_S", 120.0), 15.0, 3600.0)));
    // The XrInstance stays: the process leaves through _exit (TriAevumSdlActivity.onDestroy).
}

TickInfo LatestTick() {
    std::lock_guard lock(gTickMutex);
    return gTick;
}

bool WaitDisplayTick(uint64_t tick, uint32_t timeoutMs) {
    std::unique_lock lock(gTickMutex);
    if (!gTick.Valid) return false;
    return gTickChanged.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                 [&] { return !gTick.Valid || gTick.Tick >= tick; }) &&
           gTick.Valid && gTick.Tick >= tick;
}

bool PredictEyes(int64_t displayTimeNs, Fast::StereoViews* out) {
    const auto& g = G();
    if (g.WorldSpace == XR_NULL_HANDLE || g.SessionStopped.load() || !g.Running.load() || displayTimeNs <= 0)
        return false;
    XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = displayTimeNs;
    info.space = g.WorldSpace;
    XrViewState state{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    if (XR_FAILED(xrLocateViews(g.Session, &info, &state, 2, &count, views)) || count != 2U) return false;
    constexpr XrViewStateFlags kValid = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if ((state.viewStateFlags & kValid) != kValid) return false;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        auto& target = out->Eye[eye];
        const auto& pose = views[eye].pose;
        target.Orientation[0] = pose.orientation.x;
        target.Orientation[1] = pose.orientation.y;
        target.Orientation[2] = pose.orientation.z;
        target.Orientation[3] = pose.orientation.w;
        target.Position[0] = pose.position.x;
        target.Position[1] = pose.position.y;
        target.Position[2] = pose.position.z;
        target.TanLeft = std::tan(views[eye].fov.angleLeft);
        target.TanRight = std::tan(views[eye].fov.angleRight);
        target.TanUp = std::tan(views[eye].fov.angleUp);
        target.TanDown = std::tan(views[eye].fov.angleDown);
    }
    out->DisplayTimeNs = displayTimeNs;
    out->Valid = true;
    return true;
}

bool LocateHead(int64_t displayTimeNs, HeadPose* out) {
    const auto& g = G();
    if (g.WorldSpace == XR_NULL_HANDLE || g.SessionStopped.load() || !g.Running.load() || displayTimeNs <= 0)
        return false;
    XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    location.next = &velocity;
    if (XR_FAILED(xrLocateSpace(g.ViewSpace, g.WorldSpace, displayTimeNs, &location))) return false;
    constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((location.locationFlags & kValid) != kValid) return false;
    const auto& pose = location.pose;
    *out = {};
    out->Orientation[0] = pose.orientation.x;
    out->Orientation[1] = pose.orientation.y;
    out->Orientation[2] = pose.orientation.z;
    out->Orientation[3] = pose.orientation.w;
    out->Position[0] = pose.position.x;
    out->Position[1] = pose.position.y;
    out->Position[2] = pose.position.z;
    if ((velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0U) {
        out->LinearVelocity[0] = velocity.linearVelocity.x;
        out->LinearVelocity[1] = velocity.linearVelocity.y;
        out->LinearVelocity[2] = velocity.linearVelocity.z;
    }
    out->Valid = true;
    return true;
}

PlayArea WorldPlayArea() {
    std::lock_guard lock(gPlayAreaMutex);
    return gPlayArea;
}

uint32_t TakeWorldRecenter() { return gWorldRecenters.exchange(0); }

const char* LayerModeName(LayerMode mode) {
    switch (mode) {
    case LayerMode::World: return "world";
    case LayerMode::Cinema: return "cinema";
    case LayerMode::Black: return "black";
    }
    return "?";
}

void RequestLayerMode(LayerMode mode, uint32_t fadeMs, uint32_t minRigEpoch) {
    {
        std::lock_guard lock(gLayerMutex);
        gLayerRequest.ModePending = true;
        gLayerRequest.Mode = mode;
        gLayerRequest.FadeMs = fadeMs;
        gLayerRequest.MinRigEpoch = minRigEpoch;
    }
    gRequestedLayer = static_cast<int>(mode);
}

void RequestBlink(uint32_t rigEpoch, uint32_t fadeMs) {
    std::lock_guard lock(gLayerMutex);
    gLayerRequest.BlinkPending = true;
    gLayerRequest.BlinkEpoch = rigEpoch;
    gLayerRequest.BlinkMs = fadeMs;
}

void Pulse(int hand, uint32_t milliseconds) {
    Haptic(hand == 0 || hand == 2, hand == 1 || hand == 2, static_cast<int>(milliseconds));
}

bool NoteGuestPaused(bool paused) {
    static bool current = false;
    static double since = 0.0;
    if (paused == current) return false;
    current = paused;
    gGuestPaused = paused;
    if (paused) {
        since = NowSeconds();
        const auto& g = G();
        XRLOG("TRIAEVUM_XR_PAUSE reason=%s", g.Exiting.load() ? "EXITING" : SessionStateName(g.SessionState.load()));
    } else {
        XRLOG("TRIAEVUM_XR_RESUME paused_ms=%.0f", (NowSeconds() - since) * 1000.0);
    }
    return true;
}

void NoteGuestRefresh() {
    if (gGuestPaused.load()) ++gGuestRefreshesWhilePaused;
}

void NotePacing(uint32_t targetHz, const char* frameRateMode) {
    static uint32_t loggedHz = UINT32_MAX;
    if (loggedHz == targetHz) return;
    loggedHz = targetHz;
    XRLOG("TRIAEVUM_XR_PACING target_hz=%u frame_rate_mode=%s", targetHz, frameRateMode);
}

}  // namespace TriAevumQuestXr
