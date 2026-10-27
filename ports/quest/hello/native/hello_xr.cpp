// TriAevum XR Test: the smallest immersive OpenXR + Vulkan app on the stack the
// Quest port will use (SDL2 SDLActivity, Khronos loader, XR_KHR_vulkan_enable2,
// a dedicated XR thread), plus two platform probes:
//   1. side-load: dlopen a library copied from <ext>/incoming into filesDir;
//   2. CPU: affinity mask, usable cores, max frequencies and part numbers,
//      before and after XR_EXT_performance_settings.
// Everything is logged under the tag "TriAevumXR" and shown on a HUD quad.

#include <SDL.h>

#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <sched.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "font5x7.h"

namespace {

constexpr const char* kTag = "TriAevumXR";
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kTag, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, kTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kTag, __VA_ARGS__)

#define XR_CHECK(expr)                                                                \
    do {                                                                              \
        const XrResult r_ = (expr);                                                   \
        if (XR_FAILED(r_)) {                                                          \
            LOGE("%s failed: XrResult %d (line %d)", #expr, static_cast<int>(r_), __LINE__); \
            return false;                                                             \
        }                                                                             \
    } while (0)

#define VK_CHECK(expr)                                                                \
    do {                                                                              \
        const VkResult r_ = (expr);                                                   \
        if (r_ != VK_SUCCESS) {                                                       \
            LOGE("%s failed: VkResult %d (line %d)", #expr, static_cast<int>(r_), __LINE__); \
            return false;                                                             \
        }                                                                             \
    } while (0)

std::string strf(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

double now_seconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

const char* env_or(const char* name, const char* fallback) {
    const char* v = getenv(name);
    return v && *v ? v : fallback;
}

// ------------------------------------------------------------------ math (column-major)

struct Mat4 {
    float m[16];
};

Mat4 mat_identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
    return r;
}

Mat4 mat_mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k)
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

Mat4 mat_translate(float x, float y, float z) {
    Mat4 r = mat_identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

Mat4 mat_scale(float x, float y, float z) {
    Mat4 r = mat_identity();
    r.m[0] = x;
    r.m[5] = y;
    r.m[10] = z;
    return r;
}

Mat4 mat_rot_y(float a) {
    Mat4 r = mat_identity();
    r.m[0] = std::cos(a);
    r.m[2] = -std::sin(a);
    r.m[8] = std::sin(a);
    r.m[10] = std::cos(a);
    return r;
}

Mat4 mat_rot_x(float a) {
    Mat4 r = mat_identity();
    r.m[5] = std::cos(a);
    r.m[6] = std::sin(a);
    r.m[9] = -std::sin(a);
    r.m[10] = std::cos(a);
    return r;
}

Mat4 mat_from_quat(const XrQuaternionf& q) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    Mat4 r = mat_identity();
    r.m[0] = 1 - 2 * (y * y + z * z);
    r.m[1] = 2 * (x * y + z * w);
    r.m[2] = 2 * (x * z - y * w);
    r.m[4] = 2 * (x * y - z * w);
    r.m[5] = 1 - 2 * (x * x + z * z);
    r.m[6] = 2 * (y * z + x * w);
    r.m[8] = 2 * (x * z + y * w);
    r.m[9] = 2 * (y * z - x * w);
    r.m[10] = 1 - 2 * (x * x + y * y);
    return r;
}

// Inverse of the eye pose (rigid): R^T, -R^T t.
Mat4 mat_view(const XrPosef& pose) {
    const Mat4 rot = mat_from_quat(pose.orientation);
    Mat4 r = mat_identity();
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row)
            r.m[c * 4 + row] = rot.m[row * 4 + c];
    const float t[3] = {pose.position.x, pose.position.y, pose.position.z};
    for (int row = 0; row < 3; ++row)
        r.m[12 + row] = -(r.m[0 + row] * t[0] + r.m[4 + row] * t[1] + r.m[8 + row] * t[2]);
    return r;
}

// Asymmetric projection for Vulkan clip space (y down, z in [0, 1]), as in
// the OpenXR SDK's xr_linear.h with GRAPHICS_VULKAN.
Mat4 mat_projection(const XrFovf& fov, float nearZ, float farZ) {
    const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
    const float u = std::tan(fov.angleUp), d = std::tan(fov.angleDown);
    const float w = r - l;
    const float h = d - u;
    Mat4 m{};
    m.m[0] = 2.f / w;
    m.m[8] = (r + l) / w;
    m.m[5] = 2.f / h;
    m.m[9] = (u + d) / h;
    m.m[10] = -farZ / (farZ - nearZ);
    m.m[14] = -(farZ * nearZ) / (farZ - nearZ);
    m.m[11] = -1.f;
    return m;
}

// ------------------------------------------------------------------ probes

std::string read_first_line(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    if (!f || !std::getline(f, line))
        return {};
    return line;
}

std::string khz_to_mhz(const std::string& khz) {
    if (khz.empty())
        return "?";
    return std::to_string(std::atol(khz.c_str()) / 1000);
}

// CPU probe on the calling thread: affinity, usable cores, per-core max
// frequency (hardware and current governor cap) and the CPU part numbers.
std::vector<std::string> cpu_probe(const char* label) {
    std::vector<std::string> out;
    cpu_set_t set;
    CPU_ZERO(&set);
    std::vector<int> allowed;
    unsigned long long mask = 0;
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        for (int i = 0; i < 64; ++i)
            if (CPU_ISSET(i, &set)) {
                allowed.push_back(i);
                mask |= 1ull << i;
            }
    }
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    const long conf = sysconf(_SC_NPROCESSORS_CONF);
    out.push_back(strf("CPU %s: AFFINITY 0x%llX, USABLE %zu CORES (ONLINE %ld, CONFIGURED %ld)", label, mask,
                       allowed.size(), online, conf));
    std::string maxLine = "  CPUINFO_MAX MHZ:", capLine = "  SCALING_MAX MHZ:", curLine = "  SCALING_CUR MHZ:";
    for (int cpu : allowed) {
        const std::string base = strf("/sys/devices/system/cpu/cpu%d/cpufreq/", cpu);
        maxLine += strf(" %d:%s", cpu, khz_to_mhz(read_first_line(base + "cpuinfo_max_freq")).c_str());
        capLine += strf(" %d:%s", cpu, khz_to_mhz(read_first_line(base + "scaling_max_freq")).c_str());
        curLine += strf(" %d:%s", cpu, khz_to_mhz(read_first_line(base + "scaling_cur_freq")).c_str());
    }
    out.push_back(maxLine);
    out.push_back(capLine);
    out.push_back(curLine);
    std::ifstream info("/proc/cpuinfo");
    std::string line, parts = "  CPU PART:";
    int processor = -1;
    std::map<int, std::string> partOf;
    while (std::getline(info, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string key = line.substr(0, colon);
        key.erase(key.find_last_not_of(" \t") + 1);
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        if (key == "processor")
            processor = std::atoi(value.c_str());
        else if (key == "CPU part" && processor >= 0)
            partOf[processor] = value;
    }
    if (partOf.empty())
        parts += " (NOT IN /PROC/CPUINFO)";
    for (const auto& [cpu, part] : partOf)
        parts += strf(" %d:%s", cpu, part.c_str());
    out.push_back(parts);
    for (const auto& l : out)
        LOGI("%s", l.c_str());
    return out;
}

// Side-load probe. HelloXrActivity has copied <ext>/incoming/libprobe.so to
// filesDir/probe/libprobe.so and made it read-only.
std::vector<std::string> sideload_probe() {
    std::vector<std::string> out;
    const char* copy = env_or("TRIAEVUM_PROBE_COPY", "(activity reported nothing)");
    const char* sdcard = getenv("TRIAEVUM_PROBE_SDCARD");
    const char* lib = getenv("TRIAEVUM_PROBE_LIB");
    out.push_back(strf("PROBE COPY: %s", copy));
    using ProbeFn = int (*)();
    if (sdcard && access(sdcard, F_OK) == 0) {
        dlerror();
        void* h = dlopen(sdcard, RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            const char* err = dlerror();
            out.push_back(strf("PROBE /SDCARD DLOPEN: FAILED AS EXPECTED: %s", err ? err : "?"));
        } else {
            auto fn = reinterpret_cast<ProbeFn>(dlsym(h, "triaevum_probe"));
            out.push_back(strf("PROBE /SDCARD DLOPEN: LOADED (UNEXPECTED), TRIAEVUM_PROBE() = %d", fn ? fn() : -1));
            dlclose(h);
        }
    } else {
        out.push_back("PROBE /SDCARD DLOPEN: SKIPPED, NO INCOMING/LIBPROBE.SO");
    }
    if (lib) {
        dlerror();
        void* h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            const char* err = dlerror();
            out.push_back(strf("PROBE FILESDIR DLOPEN: FAILED: %s", err ? err : "?"));
        } else {
            auto fn = reinterpret_cast<ProbeFn>(dlsym(h, "triaevum_probe"));
            if (!fn)
                out.push_back(strf("PROBE FILESDIR DLOPEN: OK BUT NO TRIAEVUM_PROBE SYMBOL: %s", dlerror()));
            else {
                const int v = fn();
                out.push_back(strf("PROBE FILESDIR DLOPEN: OK, TRIAEVUM_PROBE() = %d%s", v, v == 42 ? " (PASS)" : " (WRONG)"));
            }
        }
    } else {
        out.push_back("PROBE FILESDIR DLOPEN: SKIPPED, NOTHING COPIED (PUSH LIBPROBE.SO, RELAUNCH)");
    }
    for (const auto& l : out)
        LOGI("%s", l.c_str());
    return out;
}

// ------------------------------------------------------------------ shaders

const uint32_t kSceneVert[] = {
#include "scene.vert.inc"
};
const uint32_t kSceneFrag[] = {
#include "scene.frag.inc"
};

struct Vertex {
    float pos[3];
    float normal[3];
};

std::vector<Vertex> make_geometry() {
    std::vector<Vertex> v;
    // Cube: 6 faces x 2 triangles, unit size centred on the origin.
    const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto& f : n) {
        // Two tangents spanning the face.
        float t1[3] = {f[1], f[2], f[0]};
        float t2[3] = {f[1] * t1[2] - f[2] * t1[1], f[2] * t1[0] - f[0] * t1[2], f[0] * t1[1] - f[1] * t1[0]};
        auto corner = [&](float a, float b) {
            Vertex vx{};
            for (int i = 0; i < 3; ++i) {
                vx.pos[i] = 0.5f * f[i] + 0.5f * a * t1[i] + 0.5f * b * t2[i];
                vx.normal[i] = f[i];
            }
            return vx;
        };
        v.push_back(corner(-1, -1));
        v.push_back(corner(1, -1));
        v.push_back(corner(1, 1));
        v.push_back(corner(-1, -1));
        v.push_back(corner(1, 1));
        v.push_back(corner(-1, 1));
    }
    // Floor quad: [-1, 1]^2 at y = 0, facing up.
    const float q[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    for (const auto& c : q)
        v.push_back(Vertex{{c[0], 0.f, c[1]}, {0.f, 1.f, 0.f}});
    return v;
}

struct Push {
    Mat4 model;
    float color[4];
    float params[4];
};

const float kPalette[][3] = {
    {0.90f, 0.45f, 0.12f}, {0.20f, 0.65f, 0.95f}, {0.35f, 0.85f, 0.35f},
    {0.90f, 0.25f, 0.45f}, {0.95f, 0.85f, 0.25f}, {0.70f, 0.45f, 0.95f},
};

// ------------------------------------------------------------------ app state

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::vector<XrSwapchainImageVulkan2KHR> images;
    std::vector<VkImageView> views;
};

struct FrameSlot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer ubo = VK_NULL_HANDLE;
    VkDeviceMemory uboMem = VK_NULL_HANDLE;
    void* uboMap = nullptr;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    void* stagingMap = nullptr;
};

constexpr int32_t kHudWidth = 1024, kHudHeight = 512;

struct App {
    // OpenXR
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    bool hasRefresh = false, hasPerf = false, hasThread = false, hasLocalFloor = false;
    PFN_xrGetVulkanGraphicsRequirements2KHR getVkReqs = nullptr;
    PFN_xrCreateVulkanInstanceKHR createVkInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR getVkDevice = nullptr;
    PFN_xrCreateVulkanDeviceKHR createVkDevice = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB enumRates = nullptr;
    PFN_xrRequestDisplayRefreshRateFB requestRate = nullptr;
    PFN_xrGetDisplayRefreshRateFB getRate = nullptr;
    PFN_xrPerfSettingsSetPerformanceLevelEXT setPerf = nullptr;
    PFN_xrSetAndroidApplicationThreadKHR setThread = nullptr;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false, focused = false, exitRequested = false;

    // Vulkan
    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t family = 0, queueCount = 0, xrQueueIndex = 0;
    VkQueue queue = VK_NULL_HANDLE;  // the binding's queue; the XR thread renders on it
    VkPhysicalDeviceMemoryProperties memProps{};
    PFN_vkCmdBeginRendering beginRendering = nullptr;
    PFN_vkCmdEndRendering endRendering = nullptr;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer vbo = VK_NULL_HANDLE;
    VkDeviceMemory vboMem = VK_NULL_HANDLE;
    uint32_t vertexCount = 0;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkImage depth = VK_NULL_HANDLE;
    VkDeviceMemory depthMem = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    FrameSlot slots[2];
    uint32_t slot = 0;

    Swapchain eye, hud;
    std::vector<XrViewConfigurationView> viewCfg;

    // Input
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction moveAction = XR_NULL_HANDLE, colourAction = XR_NULL_HANDLE, exitAction = XR_NULL_HANDLE,
             buzzAction = XR_NULL_HANDLE;

    // Scene
    float floorY = 0.f;
    float cubeX = 0.f, cubeZ = -1.2f, angle = 0.f;
    int colour = 0;

    // HUD and status
    bool hudEverReleased = false;
    double hudNext = 0.0;
    std::string runtimeLine, vkLine, perfLine = "PERF: NOT SET YET", threadLine = "THREAD HINTS: NOT SET YET";
    std::string ratesLine = "?";
    float requestedRate = 0.f, currentRate = 0.f;
    std::vector<std::string> probeLines, cpuBefore, cpuAfter;
    double perfSetAt = -1.0;
    double fps = 0.0, fpsStart = 0.0;
    int fpsFrames = 0;
};

std::atomic<bool> g_quit{false};
JavaVM* g_vm = nullptr;
jobject g_activity = nullptr;
pid_t g_mainTid = 0;

template <typename T>
T xr_proc(XrInstance instance, const char* name) {
    PFN_xrVoidFunction fn = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &fn)))
        return nullptr;
    return reinterpret_cast<T>(fn);
}

// ------------------------------------------------------------------ OpenXR instance

bool create_instance(App& a) {
    auto initLoader = xr_proc<PFN_xrInitializeLoaderKHR>(XR_NULL_HANDLE, "xrInitializeLoaderKHR");
    if (!initLoader) {
        LOGE("xrInitializeLoaderKHR missing");
        return false;
    }
    XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    li.applicationVM = g_vm;
    li.applicationContext = g_activity;
    XR_CHECK(initLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&li)));

    uint32_t n = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr));
    std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data()));
    auto has = [&](const char* name) {
        for (const auto& p : props)
            if (!std::strcmp(p.extensionName, name))
                return true;
        return false;
    };
    {
        std::string list;
        for (size_t i = 0; i < props.size(); ++i) {
            list += strf("%s%s", list.empty() ? "" : ", ", props[i].extensionName);
            if ((i + 1) % 10 == 0 || i + 1 == props.size()) {
                LOGI("OpenXR extensions offered: %s", list.c_str());
                list.clear();
            }
        }
    }
    if (!has(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) {
        LOGE("runtime lacks XR_KHR_vulkan_enable2");
        return false;
    }
    std::vector<const char*> exts = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME};
    a.hasRefresh = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    a.hasPerf = has(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    a.hasThread = has(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
    a.hasLocalFloor = has(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);
    if (a.hasRefresh)
        exts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (a.hasPerf)
        exts.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if (a.hasThread)
        exts.push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
    if (a.hasLocalFloor)
        exts.push_back(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);

    XrInstanceCreateInfoAndroidKHR aci{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    aci.applicationVM = g_vm;
    aci.applicationActivity = g_activity;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &aci;
    std::strncpy(ci.applicationInfo.applicationName, "TriAevum XR Test", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(ci.applicationInfo.engineName, "TriAevum", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.enabledExtensionNames = exts.data();
    XR_CHECK(xrCreateInstance(&ci, &a.instance));

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    XR_CHECK(xrGetInstanceProperties(a.instance, &ip));
    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(a.instance, &si, &a.systemId));
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    XR_CHECK(xrGetSystemProperties(a.instance, a.systemId, &sp));
    a.runtimeLine = strf("OPENXR: %s %u.%u.%u ON %s", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
                         XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion), sp.systemName);
    LOGI("%s; refresh-rate %d, perf-settings %d, thread-settings %d, local-floor %d", a.runtimeLine.c_str(),
         a.hasRefresh, a.hasPerf, a.hasThread, a.hasLocalFloor);

    a.getVkReqs = xr_proc<PFN_xrGetVulkanGraphicsRequirements2KHR>(a.instance, "xrGetVulkanGraphicsRequirements2KHR");
    a.createVkInstance = xr_proc<PFN_xrCreateVulkanInstanceKHR>(a.instance, "xrCreateVulkanInstanceKHR");
    a.getVkDevice = xr_proc<PFN_xrGetVulkanGraphicsDevice2KHR>(a.instance, "xrGetVulkanGraphicsDevice2KHR");
    a.createVkDevice = xr_proc<PFN_xrCreateVulkanDeviceKHR>(a.instance, "xrCreateVulkanDeviceKHR");
    if (a.hasRefresh) {
        a.enumRates = xr_proc<PFN_xrEnumerateDisplayRefreshRatesFB>(a.instance, "xrEnumerateDisplayRefreshRatesFB");
        a.requestRate = xr_proc<PFN_xrRequestDisplayRefreshRateFB>(a.instance, "xrRequestDisplayRefreshRateFB");
        a.getRate = xr_proc<PFN_xrGetDisplayRefreshRateFB>(a.instance, "xrGetDisplayRefreshRateFB");
    }
    if (a.hasPerf)
        a.setPerf = xr_proc<PFN_xrPerfSettingsSetPerformanceLevelEXT>(a.instance, "xrPerfSettingsSetPerformanceLevelEXT");
    if (a.hasThread)
        a.setThread = xr_proc<PFN_xrSetAndroidApplicationThreadKHR>(a.instance, "xrSetAndroidApplicationThreadKHR");
    return a.getVkReqs && a.createVkInstance && a.getVkDevice && a.createVkDevice;
}

// ------------------------------------------------------------------ Vulkan through XR_KHR_vulkan_enable2

bool create_vulkan(App& a) {
    XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    XR_CHECK(a.getVkReqs(a.instance, a.systemId, &req));
    LOGI("Vulkan requirements: API %u.%u to %u.%u", XR_VERSION_MAJOR(req.minApiVersionSupported),
         XR_VERSION_MINOR(req.minApiVersionSupported), XR_VERSION_MAJOR(req.maxApiVersionSupported),
         XR_VERSION_MINOR(req.maxApiVersionSupported));

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "TriAevum XR Test";
    app.applicationVersion = 1;
    app.pEngineName = "TriAevum";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    XrVulkanInstanceCreateInfoKHR xici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xici.systemId = a.systemId;
    xici.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xici.vulkanCreateInfo = &ici;
    VkResult vr = VK_SUCCESS;
    XR_CHECK(a.createVkInstance(a.instance, &xici, &a.vkInstance, &vr));
    VK_CHECK(vr);

    XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gi.systemId = a.systemId;
    gi.vulkanInstance = a.vkInstance;
    XR_CHECK(a.getVkDevice(a.instance, &gi, &a.phys));

    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(a.phys, &pp);
    vkGetPhysicalDeviceMemoryProperties(a.phys, &a.memProps);

    uint32_t nf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(a.phys, &nf, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nf);
    vkGetPhysicalDeviceQueueFamilyProperties(a.phys, &nf, fams.data());
    bool found = false;
    for (uint32_t i = 0; i < nf; ++i)
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            a.family = i;
            found = true;
            break;
        }
    if (!found) {
        LOGE("no graphics queue family");
        return false;
    }
    a.queueCount = std::min<uint32_t>(2, fams[a.family].queueCount);
    a.xrQueueIndex = a.queueCount - 1;
    if (a.family != 0 || a.queueCount < 2)
        LOGW("graphics family %u offers %u queues; wanted 2 in family 0", a.family, fams[a.family].queueCount);

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(a.phys, &f2);
    a.vkLine = strf("VULKAN: %s, API %u.%u.%u, DRIVER 0x%X, %u QUEUES IN FAMILY %u (XR ON %u), MULTIVIEW %d, "
                    "DYNAMIC RENDERING %d, SYNC2 %d",
                    pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion),
                    VK_API_VERSION_PATCH(pp.apiVersion), pp.driverVersion, a.queueCount, a.family, a.xrQueueIndex,
                    f11.multiview, f13.dynamicRendering, f13.synchronization2);
    LOGI("%s", a.vkLine.c_str());
    if (pp.apiVersion < VK_API_VERSION_1_3 || !f11.multiview || !f13.dynamicRendering) {
        LOGE("this test needs Vulkan 1.3 with multiview and dynamic rendering");
        return false;
    }

    const float prio[2] = {1.f, 1.f};
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = a.family;
    qci.queueCount = a.queueCount;
    qci.pQueuePriorities = prio;
    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan11Features e11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    e11.multiview = VK_TRUE;
    e11.pNext = &e13;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &e11;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xdci.systemId = a.systemId;
    xdci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xdci.vulkanPhysicalDevice = a.phys;
    xdci.vulkanCreateInfo = &dci;
    XR_CHECK(a.createVkDevice(a.instance, &xdci, &a.device, &vr));
    VK_CHECK(vr);
    vkGetDeviceQueue(a.device, a.family, a.xrQueueIndex, &a.queue);

    a.beginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(a.device, "vkCmdBeginRendering"));
    a.endRendering = reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(a.device, "vkCmdEndRendering"));
    if (!a.beginRendering || !a.endRendering) {
        a.beginRendering =
            reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(a.device, "vkCmdBeginRenderingKHR"));
        a.endRendering = reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(a.device, "vkCmdEndRenderingKHR"));
    }
    if (!a.beginRendering || !a.endRendering) {
        LOGE("vkCmdBeginRendering not available");
        return false;
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = a.family;
    VK_CHECK(vkCreateCommandPool(a.device, &pci, nullptr, &a.cmdPool));
    return true;
}

uint32_t find_memory(const App& a, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < a.memProps.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (a.memProps.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    return UINT32_MAX;
}

bool create_buffer(App& a, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf, VkDeviceMemory& mem,
                   void** map) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(a.device, &bci, nullptr, &buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(a.device, buf, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex =
        find_memory(a, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) {
        LOGE("no host-visible memory");
        return false;
    }
    VK_CHECK(vkAllocateMemory(a.device, &mai, nullptr, &mem));
    VK_CHECK(vkBindBufferMemory(a.device, buf, mem, 0));
    VK_CHECK(vkMapMemory(a.device, mem, 0, VK_WHOLE_SIZE, 0, map));
    return true;
}

VkShaderModule make_shader(App& a, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(a.device, &ci, nullptr, &m) != VK_SUCCESS)
        LOGE("vkCreateShaderModule failed");
    return m;
}

// ------------------------------------------------------------------ session, swapchains, pipeline

int64_t pick_format(const std::vector<int64_t>& formats) {
    for (int64_t want : {static_cast<int64_t>(VK_FORMAT_R8G8B8A8_SRGB), static_cast<int64_t>(VK_FORMAT_B8G8R8A8_SRGB)})
        if (std::find(formats.begin(), formats.end(), want) != formats.end())
            return want;
    return formats.empty() ? VK_FORMAT_R8G8B8A8_UNORM : formats.front();
}

bool create_swapchain(App& a, Swapchain& sc, int64_t format, int32_t w, int32_t h, uint32_t layers,
                      XrSwapchainUsageFlags usage) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = usage;
    ci.format = format;
    ci.sampleCount = 1;
    ci.width = static_cast<uint32_t>(w);
    ci.height = static_cast<uint32_t>(h);
    ci.faceCount = 1;
    ci.arraySize = layers;
    ci.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(a.session, &ci, &sc.handle));
    sc.width = w;
    sc.height = h;
    sc.format = static_cast<VkFormat>(format);
    uint32_t n = 0;
    XR_CHECK(xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr));
    sc.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(sc.handle, n, &n,
                                        reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data())));
    for (const auto& img : sc.images) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img.image;
        vci.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vci.format = sc.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        VkImageView view = VK_NULL_HANDLE;
        VK_CHECK(vkCreateImageView(a.device, &vci, nullptr, &view));
        sc.views.push_back(view);
    }
    LOGI("swapchain %dx%d x%u, format %lld, %u images", w, h, layers, static_cast<long long>(format), n);
    return true;
}

bool create_session(App& a) {
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = a.vkInstance;
    binding.physicalDevice = a.phys;
    binding.device = a.device;
    binding.queueFamilyIndex = a.family;
    binding.queueIndex = a.xrQueueIndex;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = a.systemId;
    XR_CHECK(xrCreateSession(a.instance, &sci, &a.session));

    // Thread hints: this thread renders; SDL's thread stands in for the game.
    if (a.setThread) {
        const XrResult r1 = a.setThread(a.session, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, gettid());
        const XrResult r2 = a.setThread(a.session, XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR, g_mainTid);
        a.threadLine = strf("THREAD HINTS: XR TID %d RENDERER_MAIN %s, SDL TID %d APPLICATION_MAIN %s", gettid(),
                            XR_SUCCEEDED(r1) ? "OK" : strf("ERR %d", r1).c_str(), g_mainTid,
                            XR_SUCCEEDED(r2) ? "OK" : strf("ERR %d", r2).c_str());
    } else {
        a.threadLine = "THREAD HINTS: XR_KHR_ANDROID_THREAD_SETTINGS NOT OFFERED";
    }
    LOGI("%s", a.threadLine.c_str());

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1.f;
    rs.referenceSpaceType = a.hasLocalFloor ? XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT : XR_REFERENCE_SPACE_TYPE_LOCAL;
    a.floorY = a.hasLocalFloor ? 0.f : -1.6f;
    XR_CHECK(xrCreateReferenceSpace(a.session, &rs, &a.space));

    uint32_t n = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(a.instance, a.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                               &n, nullptr));
    a.viewCfg.assign(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(a.instance, a.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n,
                                               &n, a.viewCfg.data()));
    if (n != 2) {
        LOGE("expected 2 views, got %u", n);
        return false;
    }
    uint32_t nfmt = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(a.session, 0, &nfmt, nullptr));
    std::vector<int64_t> formats(nfmt);
    XR_CHECK(xrEnumerateSwapchainFormats(a.session, nfmt, &nfmt, formats.data()));
    const int64_t fmt = pick_format(formats);
    const auto w = static_cast<int32_t>(a.viewCfg[0].recommendedImageRectWidth);
    const auto h = static_cast<int32_t>(a.viewCfg[0].recommendedImageRectHeight);
    if (!create_swapchain(a, a.eye, fmt, w, h, 2,
                          XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT))
        return false;
    if (!create_swapchain(a, a.hud, fmt, kHudWidth, kHudHeight, 1,
                          XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                              XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT))
        return false;

    // Depth: our own 2-layer image, never submitted.
    for (VkFormat df : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(a.phys, df, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            a.depthFormat = df;
            break;
        }
    }
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = a.depthFormat;
    ici.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 2;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    VK_CHECK(vkCreateImage(a.device, &ici, nullptr, &a.depth));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(a.device, a.depth, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_memory(a, mr.memoryTypeBits, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX)
        mai.memoryTypeIndex = find_memory(a, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(a.device, &mai, nullptr, &a.depthMem));
    VK_CHECK(vkBindImageMemory(a.device, a.depth, a.depthMem, 0));
    VkImageViewCreateInfo dvci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    dvci.image = a.depth;
    dvci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    dvci.format = a.depthFormat;
    dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 2};
    VK_CHECK(vkCreateImageView(a.device, &dvci, nullptr, &a.depthView));
    return true;
}

bool create_pipeline(App& a) {
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 1;
    dli.pBindings = &b;
    VK_CHECK(vkCreateDescriptorSetLayout(a.device, &dli, nullptr, &a.dsl));
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &a.dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(a.device, &pli, nullptr, &a.layout));

    VkShaderModule vs = make_shader(a, kSceneVert, sizeof kSceneVert);
    VkShaderModule fs = make_shader(a, kSceneFrag, sizeof kSceneFrag);
    if (!vs || !fs)
        return false;
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkVertexInputBindingDescription vb{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)},
                                               {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo pri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pri.viewMask = 0b11;
    pri.colorAttachmentCount = 1;
    pri.pColorAttachmentFormats = &a.eye.format;
    pri.depthAttachmentFormat = a.depthFormat;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &pri;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = a.layout;
    const VkResult pr = vkCreateGraphicsPipelines(a.device, VK_NULL_HANDLE, 1, &gp, nullptr, &a.pipeline);
    vkDestroyShaderModule(a.device, vs, nullptr);
    vkDestroyShaderModule(a.device, fs, nullptr);
    VK_CHECK(pr);

    const std::vector<Vertex> verts = make_geometry();
    a.vertexCount = static_cast<uint32_t>(verts.size());
    void* map = nullptr;
    if (!create_buffer(a, sizeof(Vertex) * verts.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, a.vbo, a.vboMem, &map))
        return false;
    std::memcpy(map, verts.data(), sizeof(Vertex) * verts.size());

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 2;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(a.device, &dpi, nullptr, &a.descPool));
    for (FrameSlot& s : a.slots) {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = a.cmdPool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(a.device, &cai, &s.cmd));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(a.device, &fci, nullptr, &s.fence));
        if (!create_buffer(a, sizeof(Mat4) * 2, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, s.ubo, s.uboMem, &s.uboMap))
            return false;
        if (!create_buffer(a, static_cast<VkDeviceSize>(kHudWidth) * kHudHeight * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           s.staging, s.stagingMem, &s.stagingMap))
            return false;
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = a.descPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &a.dsl;
        VK_CHECK(vkAllocateDescriptorSets(a.device, &dai, &s.set));
        VkDescriptorBufferInfo dbi{s.ubo, 0, sizeof(Mat4) * 2};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = s.set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &dbi;
        vkUpdateDescriptorSets(a.device, 1, &w, 0, nullptr);
    }
    return true;
}

// ------------------------------------------------------------------ input

bool create_actions(App& a) {
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(asci.actionSetName, "hello");
    std::strcpy(asci.localizedActionSetName, "Hello");
    XR_CHECK(xrCreateActionSet(a.instance, &asci, &a.actionSet));
    auto make = [&](const char* name, const char* label, XrActionType type, XrAction& out) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(ci.actionName, name);
        std::strcpy(ci.localizedActionName, label);
        ci.actionType = type;
        return xrCreateAction(a.actionSet, &ci, &out);
    };
    XR_CHECK(make("move", "Move cube", XR_ACTION_TYPE_VECTOR2F_INPUT, a.moveAction));
    XR_CHECK(make("colour", "Change colour", XR_ACTION_TYPE_BOOLEAN_INPUT, a.colourAction));
    XR_CHECK(make("exit", "Exit", XR_ACTION_TYPE_BOOLEAN_INPUT, a.exitAction));
    XR_CHECK(make("buzz", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, a.buzzAction));
    auto path = [&](const char* s) {
        XrPath p = XR_NULL_PATH;
        xrStringToPath(a.instance, s, &p);
        return p;
    };
    const XrActionSuggestedBinding touch[] = {
        {a.moveAction, path("/user/hand/left/input/thumbstick")},
        {a.colourAction, path("/user/hand/right/input/a/click")},
        {a.exitAction, path("/user/hand/left/input/menu/click")},
        {a.buzzAction, path("/user/hand/right/output/haptic")},
    };
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    sb.countSuggestedBindings = sizeof touch / sizeof touch[0];
    sb.suggestedBindings = touch;
    XR_CHECK(xrSuggestInteractionProfileBindings(a.instance, &sb));
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &a.actionSet;
    XR_CHECK(xrAttachSessionActionSets(a.session, &at));
    return true;
}

void update_input(App& a, float dt) {
    if (!a.focused)
        return;
    XrActiveActionSet active{a.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(a.session, &si)))
        return;
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a.moveAction;
    XrActionStateVector2f mv{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(a.session, &gi, &mv)) && mv.isActive) {
        a.cubeX = std::clamp(a.cubeX + mv.currentState.x * dt * 1.2f, -3.f, 3.f);
        a.cubeZ = std::clamp(a.cubeZ - mv.currentState.y * dt * 1.2f, -4.f, 2.f);
    }
    gi.action = a.colourAction;
    XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(a.session, &gi, &b)) && b.isActive && b.changedSinceLastSync &&
        b.currentState) {
        a.colour = (a.colour + 1) % static_cast<int>(sizeof kPalette / sizeof kPalette[0]);
        XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
        vib.amplitude = 0.8f;
        vib.duration = 80'000'000;  // 80 ms
        vib.frequency = XR_FREQUENCY_UNSPECIFIED;
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
        hi.action = a.buzzAction;
        const XrResult r = xrApplyHapticFeedback(a.session, &hi, reinterpret_cast<XrHapticBaseHeader*>(&vib));
        LOGI("A pressed: colour %d, haptic %s", a.colour, XR_SUCCEEDED(r) ? "sent" : "failed");
    }
    gi.action = a.exitAction;
    XrActionStateBoolean e{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(a.session, &gi, &e)) && e.isActive && e.changedSinceLastSync &&
        e.currentState && !a.exitRequested) {
        LOGI("menu pressed: requesting session exit");
        a.exitRequested = true;
        xrRequestExitSession(a.session);
    }
}

// ------------------------------------------------------------------ session lifecycle

void on_session_running(App& a) {
    // Refresh rate: 72 Hz unless TRIAEVUM_XR_REFRESH asks for another.
    if (a.enumRates && a.requestRate) {
        uint32_t n = 0;
        a.enumRates(a.session, 0, &n, nullptr);
        std::vector<float> rates(n);
        a.enumRates(a.session, n, &n, rates.data());
        a.ratesLine.clear();
        for (float r : rates)
            a.ratesLine += strf("%s%.0f", a.ratesLine.empty() ? "" : "/", r);
        const float want = static_cast<float>(std::atof(env_or("TRIAEVUM_XR_REFRESH", "72")));
        float best = rates.empty() ? 0.f : rates.front();
        for (float r : rates)
            if (std::fabs(r - want) < std::fabs(best - want))
                best = r;
        a.requestedRate = best;
        const XrResult r = a.requestRate(a.session, best);
        LOGI("display refresh rates %s; wanted %.0f, requested %.1f: %d", a.ratesLine.c_str(), want, best, r);
    }
    if (a.getRate)
        a.getRate(a.session, &a.currentRate);

    // CPU probe before and (a few seconds) after the performance level.
    a.cpuBefore = cpu_probe("BEFORE PERF LEVEL");
    if (a.setPerf) {
        const XrResult r = a.setPerf(a.session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
        a.perfLine = strf("PERF: CPU SUSTAINED_HIGH %s", XR_SUCCEEDED(r) ? "SET" : strf("FAILED %d", r).c_str());
        a.perfSetAt = now_seconds();
    } else {
        a.perfLine = "PERF: XR_EXT_PERFORMANCE_SETTINGS NOT OFFERED";
        a.perfSetAt = now_seconds();
    }
    LOGI("%s", a.perfLine.c_str());
}

void poll_events(App& a, bool& exitLoop) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(a.instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            LOGW("instance loss pending");
            exitLoop = true;
            break;
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& e = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
            a.state = e.state;
            a.focused = e.state == XR_SESSION_STATE_FOCUSED;
            LOGI("session state %d", static_cast<int>(e.state));
            if (e.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                const XrResult r = xrBeginSession(a.session, &bi);
                LOGI("xrBeginSession: %d", r);
                if (XR_SUCCEEDED(r)) {
                    a.running = true;
                    on_session_running(a);
                }
            } else if (e.state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(a.session);
                a.running = false;
            } else if (e.state == XR_SESSION_STATE_EXITING || e.state == XR_SESSION_STATE_LOSS_PENDING) {
                exitLoop = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            const auto& e = *reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&ev);
            a.currentRate = e.toDisplayRefreshRate;
            LOGI("display refresh rate %.1f -> %.1f", e.fromDisplayRefreshRate, e.toDisplayRefreshRate);
            break;
        }
        case XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT: {
            const auto& e = *reinterpret_cast<const XrEventDataPerfSettingsEXT*>(&ev);
            LOGI("perf settings notification: domain %d subdomain %d level %d -> %d", e.domain, e.subDomain,
                 e.fromLevel, e.toLevel);
            break;
        }
        default:
            break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ------------------------------------------------------------------ HUD

std::vector<std::pair<std::string, uint32_t>> hud_lines(const App& a) {
    constexpr uint32_t white = 0xFFFFFFFF, accent = 0xFF40B0FF, dim = 0xFFB0B0B0;
    std::vector<std::pair<std::string, uint32_t>> l;
    l.emplace_back("TRIAEVUM XR TEST", accent);
    l.emplace_back(strf("FPS %.1f   DISPLAY %.0f HZ (REQUESTED %.0f, OFFERED %s)", a.fps, a.currentRate,
                        a.requestedRate, a.ratesLine.c_str()),
                   white);
    l.emplace_back(a.runtimeLine, dim);
    l.emplace_back(a.vkLine, dim);
    l.emplace_back(strf("EYES %dX%d X2, MULTIVIEW PASS ON THE XR THREAD   %s", a.eye.width, a.eye.height,
                        a.perfLine.c_str()),
                   dim);
    l.emplace_back(a.threadLine, dim);
    l.emplace_back("", white);
    for (const auto& s : a.probeLines)
        l.emplace_back(s, white);
    l.emplace_back("", white);
    for (const auto& s : a.cpuBefore)
        l.emplace_back(s, white);
    for (const auto& s : a.cpuAfter)
        l.emplace_back(s, white);
    if (a.cpuAfter.empty())
        l.emplace_back("CPU AFTER PERF LEVEL: PENDING", dim);
    l.emplace_back("", white);
    l.emplace_back("LEFT STICK MOVES THE CUBE   A CHANGES ITS COLOUR   LEFT MENU EXITS", accent);
    return l;
}

// Text into RGBA (premultiplied) or BGRA, 2x scaled 5x7 cells, wrapped.
void draw_hud(const App& a, uint8_t* px) {
    const bool bgra = a.hud.format == VK_FORMAT_B8G8R8A8_SRGB || a.hud.format == VK_FORMAT_B8G8R8A8_UNORM;
    const uint8_t bgA = 215;
    for (int i = 0; i < kHudWidth * kHudHeight; ++i) {
        px[i * 4 + 0] = 8 * bgA / 255;
        px[i * 4 + 1] = 12 * bgA / 255;
        px[i * 4 + 2] = 18 * bgA / 255;
        px[i * 4 + 3] = bgA;
    }
    constexpr int scale = 2, cellW = 6 * scale, cellH = 9 * scale, margin = 14;
    const int cols = (kHudWidth - 2 * margin) / cellW;
    int row = 0;
    auto put = [&](int x, int y, uint32_t argb) {
        if (x < 0 || y < 0 || x >= kHudWidth || y >= kHudHeight)
            return;
        uint8_t* p = px + (static_cast<size_t>(y) * kHudWidth + x) * 4;
        const uint8_t r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
        p[0] = bgra ? b : r;
        p[1] = g;
        p[2] = bgra ? r : b;
        p[3] = 255;
    };
    for (const auto& [text, colour] : hud_lines(a)) {
        size_t pos = 0;
        do {
            const std::string chunk = text.substr(pos, static_cast<size_t>(cols));
            pos += chunk.size();
            const int y0 = margin + row * cellH;
            if (y0 + cellH > kHudHeight)
                return;
            for (size_t c = 0; c < chunk.size(); ++c) {
                const Glyph5x7& g = font5x7_glyph(chunk[c]);
                const int x0 = margin + static_cast<int>(c) * cellW;
                for (int gy = 0; gy < 7; ++gy)
                    for (int gx = 0; gx < 5; ++gx)
                        if (g.rows[gy][gx] == '#')
                            for (int sy = 0; sy < scale; ++sy)
                                for (int sx = 0; sx < scale; ++sx)
                                    put(x0 + gx * scale + sx, y0 + gy * scale + sy, colour);
            }
            ++row;
        } while (pos < text.size());
    }
}

// ------------------------------------------------------------------ frame

bool render_frame(App& a, const XrView views[2], bool updateHud) {
    FrameSlot& s = a.slots[a.slot];
    a.slot ^= 1;
    VK_CHECK(vkWaitForFences(a.device, 1, &s.fence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(a.device, 1, &s.fence));

    uint32_t eyeIndex = 0, hudIndex = 0;
    XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    XR_CHECK(xrAcquireSwapchainImage(a.eye.handle, &acq, &eyeIndex));
    XR_CHECK(xrWaitSwapchainImage(a.eye.handle, &wait));
    if (updateHud) {
        draw_hud(a, static_cast<uint8_t*>(s.stagingMap));
        XR_CHECK(xrAcquireSwapchainImage(a.hud.handle, &acq, &hudIndex));
        XR_CHECK(xrWaitSwapchainImage(a.hud.handle, &wait));
    }

    Mat4 vpm[2];
    for (int e = 0; e < 2; ++e)
        vpm[e] = mat_mul(mat_projection(views[e].fov, 0.05f, 100.f), mat_view(views[e].pose));
    std::memcpy(s.uboMap, vpm, sizeof vpm);

    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(s.cmd, &cbi));

    VkImageMemoryBarrier pre[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
    pre[0].srcAccessMask = 0;
    pre[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    pre[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    pre[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    pre[0].srcQueueFamilyIndex = pre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[0].image = a.eye.images[eyeIndex].image;
    pre[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    pre[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    pre[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    pre[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    pre[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    pre[1].srcQueueFamilyIndex = pre[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[1].image = a.depth;
    pre[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 2};
    vkCmdPipelineBarrier(s.cmd,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         0, 0, nullptr, 0, nullptr, 2, pre);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = a.eye.views[eyeIndex];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.010f, 0.012f, 0.020f, 1.f}};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = a.depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {1.f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {static_cast<uint32_t>(a.eye.width), static_cast<uint32_t>(a.eye.height)}};
    ri.layerCount = 1;
    ri.viewMask = 0b11;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    a.beginRendering(s.cmd, &ri);

    VkViewport vp{0.f, 0.f, static_cast<float>(a.eye.width), static_cast<float>(a.eye.height), 0.f, 1.f};
    VkRect2D sc{{0, 0}, {static_cast<uint32_t>(a.eye.width), static_cast<uint32_t>(a.eye.height)}};
    vkCmdSetViewport(s.cmd, 0, 1, &vp);
    vkCmdSetScissor(s.cmd, 0, 1, &sc);
    vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.pipeline);
    vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, a.layout, 0, 1, &s.set, 0, nullptr);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(s.cmd, 0, 1, &a.vbo, &off);

    Push floor{};
    floor.model = mat_mul(mat_translate(0.f, a.floorY, 0.f), mat_scale(10.f, 1.f, 10.f));
    floor.params[0] = 1.f;
    vkCmdPushConstants(s.cmd, a.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof floor,
                       &floor);
    vkCmdDraw(s.cmd, 6, 1, 36, 0);

    Push cube{};
    cube.model = mat_mul(mat_translate(a.cubeX, a.floorY + 1.1f, a.cubeZ),
                         mat_mul(mat_rot_y(a.angle), mat_mul(mat_rot_x(a.angle * 0.6f), mat_scale(0.3f, 0.3f, 0.3f))));
    for (int i = 0; i < 3; ++i)
        cube.color[i] = kPalette[a.colour][i];
    cube.color[3] = 1.f;
    vkCmdPushConstants(s.cmd, a.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof cube,
                       &cube);
    vkCmdDraw(s.cmd, 36, 1, 0, 0);
    a.endRendering(s.cmd);

    if (updateHud) {
        VkImageMemoryBarrier hb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        hb.srcAccessMask = 0;
        hb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        hb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hb.image = a.hud.images[hudIndex].image;
        hb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &hb);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {kHudWidth, kHudHeight, 1};
        vkCmdCopyBufferToImage(s.cmd, s.staging, hb.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        hb.dstAccessMask = 0;  // the compositor's reads are ordered by xrReleaseSwapchainImage
        hb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        hb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &hb);
    }
    VK_CHECK(vkEndCommandBuffer(s.cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    VK_CHECK(vkQueueSubmit(a.queue, 1, &si, s.fence));

    XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_CHECK(xrReleaseSwapchainImage(a.eye.handle, &rel));
    if (updateHud) {
        XR_CHECK(xrReleaseSwapchainImage(a.hud.handle, &rel));
        a.hudEverReleased = true;
    }
    return true;
}

bool frame(App& a) {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XR_CHECK(xrWaitFrame(a.session, &wi, &fs));
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    XR_CHECK(xrBeginFrame(a.session, &bi));

    const double t = now_seconds();
    const float dt = fs.predictedDisplayPeriod > 0 ? static_cast<float>(fs.predictedDisplayPeriod) * 1e-9f : 1.f / 72.f;
    update_input(a, dt);
    a.angle += dt * 0.9f;
    if (a.perfSetAt >= 0.0 && a.cpuAfter.empty() && t - a.perfSetAt > 3.0)
        a.cpuAfter = cpu_probe("3 S AFTER PERF LEVEL");

    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (fs.shouldRender) {
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = a.space;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t n = 0;
        XR_CHECK(xrLocateViews(a.session, &li, &vs, 2, &n, views));
        if ((vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
            (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
            const bool hudNow = t >= a.hudNext;
            if (hudNow)
                a.hudNext = t + 0.5;
            if (!render_frame(a, views, hudNow))
                return false;
            for (int e = 0; e < 2; ++e) {
                pv[e].pose = views[e].pose;
                pv[e].fov = views[e].fov;
                pv[e].subImage.swapchain = a.eye.handle;
                pv[e].subImage.imageRect = {{0, 0}, {a.eye.width, a.eye.height}};
                pv[e].subImage.imageArrayIndex = static_cast<uint32_t>(e);
            }
            proj.space = a.space;
            proj.viewCount = 2;
            proj.views = pv;
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj));
        }
    }
    if (a.hudEverReleased) {
        // World-locked, up and to the left of the cube, turned towards the user.
        const float yaw = 0.50f;
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quad.space = a.space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = a.hud.handle;
        quad.subImage.imageRect = {{0, 0}, {kHudWidth, kHudHeight}};
        quad.pose.orientation = {0.f, std::sin(yaw / 2), 0.f, std::cos(yaw / 2)};
        quad.pose.position = {-1.0f, a.floorY + 1.45f, -1.7f};
        quad.size = {1.3f, 0.65f};
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad));
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = static_cast<uint32_t>(layers.size());
    ei.layers = layers.data();
    XR_CHECK(xrEndFrame(a.session, &ei));

    ++a.fpsFrames;
    if (a.fpsStart == 0.0)
        a.fpsStart = t;
    if (t - a.fpsStart >= 1.0) {
        a.fps = a.fpsFrames / (t - a.fpsStart);
        if (static_cast<int>(t) % 10 == 0)
            LOGI("fps %.1f, display %.0f Hz", a.fps, a.currentRate);
        a.fpsFrames = 0;
        a.fpsStart = t;
    }
    return true;
}

void teardown(App& a) {
    LOGI("teardown");
    if (a.device)
        vkDeviceWaitIdle(a.device);
    for (Swapchain* sc : {&a.eye, &a.hud}) {
        for (VkImageView v : sc->views)
            vkDestroyImageView(a.device, v, nullptr);
        if (sc->handle)
            xrDestroySwapchain(sc->handle);
    }
    if (a.device) {
        for (FrameSlot& s : a.slots) {
            vkDestroyFence(a.device, s.fence, nullptr);
            vkDestroyBuffer(a.device, s.ubo, nullptr);
            vkFreeMemory(a.device, s.uboMem, nullptr);
            vkDestroyBuffer(a.device, s.staging, nullptr);
            vkFreeMemory(a.device, s.stagingMem, nullptr);
        }
        vkDestroyImageView(a.device, a.depthView, nullptr);
        vkDestroyImage(a.device, a.depth, nullptr);
        vkFreeMemory(a.device, a.depthMem, nullptr);
        vkDestroyBuffer(a.device, a.vbo, nullptr);
        vkFreeMemory(a.device, a.vboMem, nullptr);
        vkDestroyPipeline(a.device, a.pipeline, nullptr);
        vkDestroyPipelineLayout(a.device, a.layout, nullptr);
        vkDestroyDescriptorPool(a.device, a.descPool, nullptr);
        vkDestroyDescriptorSetLayout(a.device, a.dsl, nullptr);
        vkDestroyCommandPool(a.device, a.cmdPool, nullptr);
    }
    if (a.actionSet)
        xrDestroyActionSet(a.actionSet);
    if (a.space)
        xrDestroySpace(a.space);
    if (a.session)
        xrDestroySession(a.session);
    if (a.device)
        vkDestroyDevice(a.device, nullptr);
    if (a.vkInstance)
        vkDestroyInstance(a.vkInstance, nullptr);
    if (a.instance)
        xrDestroyInstance(a.instance);
    LOGI("teardown done");
}

void xr_thread_main() {
    JNIEnv* env = nullptr;
    g_vm->AttachCurrentThread(&env, nullptr);
    LOGI("XR thread tid %d", gettid());
    static App a;  // never destroyed: the process leaves through _exit
    a.probeLines = sideload_probe();
    bool ok = create_instance(a) && create_vulkan(a) && create_session(a) && create_pipeline(a) && create_actions(a);
    if (!ok)
        LOGE("XR setup failed; see the errors above");
    bool exitLoop = !ok;
    while (!exitLoop) {
        poll_events(a, exitLoop);
        if (exitLoop)
            break;
        if (g_quit.load() && !a.exitRequested) {
            a.exitRequested = true;
            if (a.running)
                xrRequestExitSession(a.session);
            else
                break;
        }
        if (!a.running) {
            usleep(10'000);
            continue;
        }
        if (!frame(a)) {
            LOGE("frame failed; leaving");
            break;
        }
    }
    teardown(a);
    LOGI("exit");
    _exit(0);
}

}  // namespace

int main(int, char*[]) {
    LOGI("SDL_main: TriAevum XR Test");
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
    if (SDL_Init(SDL_INIT_EVENTS) != 0)
        LOGW("SDL_Init: %s", SDL_GetError());
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    env->GetJavaVM(&g_vm);
    auto local = static_cast<jobject>(SDL_AndroidGetActivity());
    g_activity = env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    g_mainTid = gettid();

    std::thread xr(xr_thread_main);
    xr.detach();
    double quitAt = 0.0;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT || e.type == SDL_APP_TERMINATING) {
                LOGI("SDL quit event %u", e.type);
                g_quit = true;
            }
        if (g_quit.load()) {
            if (quitAt == 0.0)
                quitAt = now_seconds();
            else if (now_seconds() - quitAt > 3.0) {
                LOGW("XR thread did not exit in 3 s; leaving");
                _exit(0);
            }
        }
        SDL_Delay(20);
    }
}
