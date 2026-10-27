// TriAevum Quest immersive mode: OpenXR instance and session, the XR frame thread (events, state
// machine, quad layer, screen placement), performance/refresh/thread hints, pause notes and exit.
// q3-spec §4.2, §4.6, §6.
#include "quest_xr_internal.h"
#include "quest_xr_input_logic.h"

#include <SDL2/SDL.h>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
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

// ---- watchdog: no shutdown wait may hang the process (q3-spec §4.6, §6.6)
void ArmWatchdog(const char* stage) {
    static std::atomic<bool> armed{false};
    if (armed.exchange(true)) return;
    std::thread([stage] {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        XRLOG("TRIAEVUM_XR_EXIT stage=watchdog armed_at=%s", stage);
        std::fflush(nullptr);
        _exit(0);
    }).detach();
}

void PushQuit() {
    SDL_Event quit{};
    quit.type = SDL_QUIT;
    if (SDL_PushEvent(&quit) < 0) XRWARN("TRIAEVUM_XR_EXIT push_quit_failed error=\"%s\"", SDL_GetError());
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
        const float want = static_cast<float>(EnvDouble("TRIAEVUM_XR_REFRESH", 72.0));
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
        g.Focused = false;
        g.Exiting = true;
        g.Stop = true;
        if (!g.ShutdownRequested.load()) {
            // Quit from the system menu (or a lost session): the game loop ends through SDL_QUIT,
            // writes its summary, and Shutdown destroys the XR objects (StopSession).
            PushQuit();
            XRLOG("TRIAEVUM_XR_EXIT stage=quit_event state=%s", SessionStateName(next));
            ArmWatchdog("quit_event");
        }
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
            g.Exiting = true;
            g.Stop = true;
            if (!g.ShutdownRequested.load()) {
                PushQuit();
                XRLOG("TRIAEVUM_XR_EXIT stage=quit_event state=instance_loss_pending");
                ArmWatchdog("instance_loss");
            }
            break;
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            ChangeState(reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state);
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            const auto& change = reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(event);
            XRLOG("TRIAEVUM_XR_EVENT reference_space_change space=%d", static_cast<int>(change.referenceSpaceType));
            if (change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                gPlaced = false;
                gPlaceAfter = change.changeTime;
                gPlaceReason = "space_change";
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
          "guest_refreshes_while_paused=%llu refresh_hz=%.0f",
          SessionStateName(g.SessionState.load()), static_cast<unsigned long long>(frames.DisplayFrames),
          static_cast<unsigned long long>(frames.ShouldRenderFalse), static_cast<unsigned long long>(sink.Copies),
          static_cast<unsigned long long>(sink.Publications),
          static_cast<unsigned long long>(sink.StaleDisplayFrames),
          static_cast<unsigned long long>(sink.IncompleteSkips),
          static_cast<unsigned long long>(sink.PublicationsDropped),
          static_cast<unsigned long long>(sink.SwapchainWaitTimeouts), frames.WaitFrameMsMax, frames.BeginFrameMsMax,
          sink.CopyWaitMsMax, gGuestPaused.load() ? 1 : 0,
          static_cast<unsigned long long>(gGuestRefreshesWhilePaused.load()), g.RefreshHz.load());
    frames = {};
}

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void XrThreadMain() {
    auto& g = G();
    pthread_setname_np(pthread_self(), "TriAevumXR");
    ApplyThreadHint(XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, gettid());
    XRLOG("TRIAEVUM_XR_THREAD_START tid=%d", gettid());
    FrameStats frames;
    double nextStats = NowSeconds() + kStatsPeriodSeconds;
    bool wasFocused = false;
    while (!g.Stop.load()) {
        ApplyPendingThreadHints();
        PollEvents();
        if (g.Stop.load()) break;
        if (NowSeconds() >= nextStats) {
            LogStats(frames);
            nextStats = NowSeconds() + kStatsPeriodSeconds;
        }
        if (!g.Running.load()) {
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
        if (frameState.shouldRender) {
            CopyLatest();
        } else {
            ++frames.ShouldRenderFalse;
        }
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        const XrCompositionLayerBaseHeader* layers[1] = {};
        uint32_t layerCount = 0;
        if (frameState.shouldRender && QuadReleased() && gPlaced) {
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

    vkGetDeviceQueue(device, queueFamily, xrQueueIndex, &g.Queue);
    if (g.Queue == VK_NULL_HANDLE) Fail(Format("no Vulkan queue %u in family %u", xrQueueIndex, queueFamily));
    try {
        CreateQuadSwapchain();
        CreateCopyResources();
        CreateActions();
    } catch (const std::exception& error) {
        Fail(error.what());
    }
    XRLOG("TRIAEVUM_XR_SESSION created queue_family=%u renderer_queue=0 xr_queue=%u", queueFamily, xrQueueIndex);
    std::error_code ignored;
    std::filesystem::remove(ExternalPath("logs/xr-start-failed.txt"), ignored);
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
    static const bool runUnfocused = EnvFlag("TRIAEVUM_XR_RUN_UNFOCUSED");
    return g.Session != XR_NULL_HANDLE && g.Running.load() && (g.Focused.load() || runUnfocused) &&
           !g.Exiting.load() && !g.Stop.load() && !g.SessionStopped.load();
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
    g.ViewSpace = g.LocalSpace = XR_NULL_HANDLE;
    if (g.Session != XR_NULL_HANDLE) {
        const XrResult result = xrDestroySession(g.Session);
        XRLOG("TRIAEVUM_XR_EXIT stage=session_destroyed result=%d", static_cast<int>(result));
        g.Session = XR_NULL_HANDLE;
    }
    // The XrInstance stays: the process leaves through _exit (TriAevumSdlActivity.onDestroy).
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
