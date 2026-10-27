// TriAevum Quest immersive mode: the quad swapchain, the renderer's private targets and their
// publication, the copy into the swapchain on the XR queue, the timelines and the XR queue lock.
// q3-spec §4.1, §5.
#include "quest_xr_internal.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace TriAevumQuestXr {

namespace {

// ---- the XR queue lock
std::mutex gQueueMutex;
std::atomic<double> gLastLockLog{0.0};

// ---- swapchain and copy resources (created on the game thread before the XR thread starts)
struct CopySlot {
    VkCommandBuffer Command = VK_NULL_HANDLE;
    VkFence Fence = VK_NULL_HANDLE;
};
XrSwapchain gSwapchain = XR_NULL_HANDLE;
VkFormat gSwapchainFormat = VK_FORMAT_UNDEFINED;
VkFormat gTargetFormat = VK_FORMAT_R8G8B8A8_UNORM;
VkExtent2D gSwapchainExtent{};
std::vector<XrSwapchainImageVulkan2KHR> gSwapchainImages;
std::vector<CopySlot> gCopySlots;
VkCommandPool gCommandPool = VK_NULL_HANDLE;
VkSemaphore gRenderTimeline = VK_NULL_HANDLE;
VkSemaphore gCopyTimeline = VK_NULL_HANDLE;
PFN_vkWaitSemaphores gWaitSemaphores = nullptr;
PFN_vkGetSemaphoreCounterValue gGetSemaphoreCounterValue = nullptr;
std::atomic<bool> gQuadReleased{false};

// ---- the sink, under gSink
struct Target {
    VkImage Image = VK_NULL_HANDLE;
    uint64_t LastCopyValue = 0;
    bool Copying = false;
};
struct Publication {
    int Image = -1;
    uint64_t RenderValue = 0;
    uint64_t Serial = 0;
};
std::mutex gSink;
std::condition_variable gSinkChanged;
std::vector<Target> gTargets;
VkExtent2D gTargetExtent{};
Publication gLatest;
uint64_t gSerial = 0;
uint64_t gShownSerial = 0;
SinkStats gStats;

// ---- XR thread only
int gPendingAcquire = -1;    // acquired and not yet released
bool gPendingWaited = false;  // xrWaitSwapchainImage succeeded for gPendingAcquire
uint64_t gCopyValue = 0;
bool gFirstImageLogged = false;

VkFormat UnormTwin(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return format;
    }
}

void Require(VkResult result, const char* call) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(call) + " failed: VkResult " + std::to_string(static_cast<int>(result)));
}

template <typename T>
T DeviceProc(VkDevice device, const char* name, const char* alias) {
    auto function = reinterpret_cast<T>(vkGetDeviceProcAddr(device, name));
    if (function == nullptr) function = reinterpret_cast<T>(vkGetDeviceProcAddr(device, alias));
    return function;
}

// Waits on the copy timeline in 250 ms slices; never returns while the copy may still read.
void WaitCopyTimeline(uint64_t value, const char* what) {
    if (value == 0U || gCopyTimeline == VK_NULL_HANDLE || gWaitSemaphores == nullptr) return;
    VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait.semaphoreCount = 1;
    wait.pSemaphores = &gCopyTimeline;
    wait.pValues = &value;
    const double start = NowSeconds();
    for (;;) {
        const VkResult result = gWaitSemaphores(G().Device, &wait, 250'000'000ULL);
        if (result == VK_SUCCESS) break;
        if (result != VK_TIMEOUT) {
            XRERR("TRIAEVUM_XR_ERROR call=vkWaitSemaphores(copy) result=%d", static_cast<int>(result));
            break;
        }
        XRWARN("TRIAEVUM_XR_WAIT_TIMEOUT what=%s value=%llu waited_ms=%.0f", what,
               static_cast<unsigned long long>(value), (NowSeconds() - start) * 1000.0);
    }
    const double waitedMs = (NowSeconds() - start) * 1000.0;
    std::lock_guard lock(gSink);
    gStats.CopyWaitMsMax = std::max(gStats.CopyWaitMsMax, waitedMs);
}

void ClearCopying(int image) {
    {
        std::lock_guard lock(gSink);
        if (image >= 0 && static_cast<size_t>(image) < gTargets.size()) gTargets[image].Copying = false;
    }
    gSinkChanged.notify_all();
}

void Barrier(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
             VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

}  // namespace

QueueLock::QueueLock(const char* holder) : mHolder(holder) {
    gQueueMutex.lock();
    mStart = std::chrono::steady_clock::now();
}

QueueLock::~QueueLock() {
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mStart).count();
    gQueueMutex.unlock();
    if (ms > 5.0) {
        const double now = NowSeconds();
        double last = gLastLockLog.load();
        if (now - last >= 2.0 && gLastLockLog.compare_exchange_strong(last, now))
            XRLOG("TRIAEVUM_XR_QUEUE_LOCK holder=%s ms=%.1f", mHolder, ms);
    }
}

VkResult DeviceWaitIdleLocked(VkDevice device) {
    if (!Requested()) return vkDeviceWaitIdle(device);  // the flat panel: no XR queue user
    QueueLock lock("renderer");
    return vkDeviceWaitIdle(device);
}

VkFormat TargetFormat() { return gTargetFormat; }
XrSwapchain QuadSwapchain() { return gSwapchain; }
VkExtent2D QuadExtent() { return gSwapchainExtent; }
bool QuadReleased() { return gQuadReleased.load(); }
VkSemaphore RenderTimeline() { return gRenderTimeline; }
VkSemaphore CopyTimeline() { return gCopyTimeline; }

void CreateQuadSwapchain() {
    auto& g = G();
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateSwapchainFormats(g.Session, 0, &count, nullptr)))
        throw std::runtime_error("xrEnumerateSwapchainFormats failed");
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(g.Session, count, &count, formats.data());
    const auto offered = [&](int64_t format) { return std::find(formats.begin(), formats.end(), format) != formats.end(); };
    int64_t chosen = 0;
    if (const double forced = EnvDouble("TRIAEVUM_XR_SWAPCHAIN_FORMAT", 0.0); forced > 0.0) {
        chosen = static_cast<int64_t>(forced);
        if (!offered(chosen)) {
            XRWARN("TRIAEVUM_XR_SWAPCHAIN override %lld not offered; using the default", static_cast<long long>(chosen));
            chosen = 0;
        }
    }
    for (int64_t format : {static_cast<int64_t>(VK_FORMAT_R8G8B8A8_SRGB), static_cast<int64_t>(VK_FORMAT_B8G8R8A8_SRGB),
                           static_cast<int64_t>(VK_FORMAT_R8G8B8A8_UNORM)})
        if (chosen == 0 && offered(format)) chosen = format;
    if (chosen == 0) throw std::runtime_error("the runtime offers no RGBA8/BGRA8 swapchain format");
    gSwapchainFormat = static_cast<VkFormat>(chosen);
    gTargetFormat = UnormTwin(gSwapchainFormat);
    gSwapchainExtent = PanelExtent();

    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                      XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = chosen;
    info.sampleCount = 1;
    info.width = gSwapchainExtent.width;
    info.height = gSwapchainExtent.height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    XrResult result = xrCreateSwapchain(g.Session, &info, &gSwapchain);
    if (XR_FAILED(result))
        throw std::runtime_error("xrCreateSwapchain failed: XrResult " + std::to_string(static_cast<int>(result)));
    uint32_t images = 0;
    xrEnumerateSwapchainImages(gSwapchain, 0, &images, nullptr);
    gSwapchainImages.assign(images, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    result = xrEnumerateSwapchainImages(gSwapchain, images, &images,
                                        reinterpret_cast<XrSwapchainImageBaseHeader*>(gSwapchainImages.data()));
    if (XR_FAILED(result) || images == 0U) throw std::runtime_error("xrEnumerateSwapchainImages failed");
    std::string list;
    for (int64_t format : formats) list += (list.empty() ? "" : ",") + std::to_string(format);
    XRLOG("TRIAEVUM_XR_SWAPCHAIN w=%u h=%u format=%d target_format=%d images=%u offered=%s", gSwapchainExtent.width,
          gSwapchainExtent.height, static_cast<int>(gSwapchainFormat), static_cast<int>(gTargetFormat), images,
          list.c_str());
}

void CreateCopyResources() {
    auto& g = G();
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = g.QueueFamily;
    Require(vkCreateCommandPool(g.Device, &pool, nullptr, &gCommandPool), "vkCreateCommandPool(xr)");
    gCopySlots.assign(gSwapchainImages.size(), {});
    for (auto& slot : gCopySlots) {
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = gCommandPool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        Require(vkAllocateCommandBuffers(g.Device, &allocate, &slot.Command), "vkAllocateCommandBuffers(xr)");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        Require(vkCreateFence(g.Device, &fence, nullptr, &slot.Fence), "vkCreateFence(xr)");
    }
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 0;
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphore.pNext = &type;
    Require(vkCreateSemaphore(g.Device, &semaphore, nullptr, &gRenderTimeline), "vkCreateSemaphore(render timeline)");
    Require(vkCreateSemaphore(g.Device, &semaphore, nullptr, &gCopyTimeline), "vkCreateSemaphore(copy timeline)");
    gWaitSemaphores = DeviceProc<PFN_vkWaitSemaphores>(g.Device, "vkWaitSemaphores", "vkWaitSemaphoresKHR");
    gGetSemaphoreCounterValue =
        DeviceProc<PFN_vkGetSemaphoreCounterValue>(g.Device, "vkGetSemaphoreCounterValue", "vkGetSemaphoreCounterValueKHR");
    if (gWaitSemaphores == nullptr || gGetSemaphoreCounterValue == nullptr)
        throw std::runtime_error("timeline semaphore commands are unavailable");
}

void AttachTargets(const VkImage* images, uint32_t count, VkExtent2D extent) {
    {
        std::lock_guard lock(gSink);
        gTargets.assign(count, {});
        for (uint32_t index = 0; index < count; ++index) gTargets[index].Image = images[index];
        gTargetExtent = extent;
        gLatest = {};
    }
    XRLOG("TRIAEVUM_XR_TARGETS count=%u w=%u h=%u format=%d", count, extent.width, extent.height,
          static_cast<int>(gTargetFormat));
}

void DetachTargets() {
    if (G().SessionStopped.load()) {
        // The XR thread is joined and queue 1 is idle (StopSession): nothing reads the targets.
        std::lock_guard lock(gSink);
        gTargets.clear();
        gLatest = {};
        return;
    }
    uint64_t lastCopy = 0;
    {
        std::unique_lock lock(gSink);
        gLatest = {};
        const double start = NowSeconds();
        while (std::any_of(gTargets.begin(), gTargets.end(), [](const Target& t) { return t.Copying; })) {
            if (gSinkChanged.wait_for(lock, std::chrono::milliseconds(250)) == std::cv_status::timeout)
                XRWARN("TRIAEVUM_XR_WAIT_TIMEOUT what=detach_copying waited_ms=%.0f", (NowSeconds() - start) * 1000.0);
        }
        for (const auto& target : gTargets) lastCopy = std::max(lastCopy, target.LastCopyValue);
    }
    WaitCopyTimeline(lastCopy, "detach");
    std::lock_guard lock(gSink);
    gTargets.clear();
}

uint64_t WaitTargetFree(uint32_t image) {
    uint64_t lastCopy = 0;
    {
        std::unique_lock lock(gSink);
        if (image >= gTargets.size()) return 0;
        if (gLatest.Image == static_cast<int>(image)) {
            // The renderer reuses the slot before the XR thread showed it: drop that frame.
            gLatest.Image = -1;
            ++gStats.PublicationsDropped;
        }
        const double start = NowSeconds();
        while (gTargets[image].Copying) {
            if (gSinkChanged.wait_for(lock, std::chrono::milliseconds(250)) == std::cv_status::timeout)
                XRWARN("TRIAEVUM_XR_WAIT_TIMEOUT what=copying image=%u waited_ms=%.0f", image,
                       (NowSeconds() - start) * 1000.0);
        }
        lastCopy = gTargets[image].LastCopyValue;
    }
    WaitCopyTimeline(lastCopy, "target_free");
    return lastCopy;
}

void Publish(uint32_t image, uint64_t renderValue) {
    std::lock_guard lock(gSink);
    if (image >= gTargets.size()) return;
    gLatest = {static_cast<int>(image), renderValue, ++gSerial};
    ++gStats.Publications;
}

void CopyLatest() {
    auto& g = G();
    Publication publication;
    VkImage source = VK_NULL_HANDLE;
    VkExtent2D region{};
    {
        std::lock_guard lock(gSink);
        if (gTargets.empty() || gLatest.Image < 0 || gLatest.Serial == gShownSerial) {
            ++gStats.StaleDisplayFrames;
            return;
        }
        uint64_t completed = 0;
        if (gGetSemaphoreCounterValue(g.Device, gRenderTimeline, &completed) != VK_SUCCESS ||
            completed < gLatest.RenderValue) {
            // Not GPU-complete yet: never release an image whose copy would wait on the renderer.
            ++gStats.IncompleteSkips;
            return;
        }
        publication = gLatest;
        Target& target = gTargets[publication.Image];
        target.Copying = true;
        source = target.Image;
        region = {std::min(gTargetExtent.width, gSwapchainExtent.width),
                  std::min(gTargetExtent.height, gSwapchainExtent.height)};
    }
    // Acquire only when no image is waiting for xrWaitSwapchainImage; a timed-out wait is retried
    // for the same image next display frame (OpenXR waits/releases the oldest acquired image).
    if (gPendingAcquire < 0) {
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult result;
        {
            QueueLock lock("xr");
            result = xrAcquireSwapchainImage(gSwapchain, &acquire, &index);
        }
        if (XR_FAILED(result)) {
            XRWARN("TRIAEVUM_XR_ERROR call=xrAcquireSwapchainImage result=%d", static_cast<int>(result));
            ClearCopying(publication.Image);
            return;
        }
        gPendingAcquire = static_cast<int>(index);
        gPendingWaited = false;
    }
    if (!gPendingWaited) {
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = 100'000'000;  // 100 ms
        const XrResult waited = xrWaitSwapchainImage(gSwapchain, &wait);
        if (waited == XR_TIMEOUT_EXPIRED || XR_FAILED(waited)) {
            if (waited != XR_TIMEOUT_EXPIRED)
                XRWARN("TRIAEVUM_XR_ERROR call=xrWaitSwapchainImage result=%d", static_cast<int>(waited));
            {
                std::lock_guard lock(gSink);
                ++gStats.SwapchainWaitTimeouts;
            }
            ClearCopying(publication.Image);
            return;
        }
        gPendingWaited = true;
    }
    const uint32_t index = static_cast<uint32_t>(gPendingAcquire);
    CopySlot& slot = gCopySlots[index];
    vkWaitForFences(g.Device, 1, &slot.Fence, VK_TRUE, UINT64_MAX);
    vkResetFences(g.Device, 1, &slot.Fence);
    vkResetCommandBuffer(slot.Command, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(slot.Command, &begin);
    const VkImage destination = gSwapchainImages[index].image;
    // A: the renderer's target (left in PRESENT_SRC by its last pass) becomes a copy source; the
    //    render-timeline wait (TRANSFER stage) orders it after the frame.
    Barrier(slot.Command, source, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // B: the swapchain image's previous contents are discarded.
    Barrier(slot.Command, destination, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // Same-size formats (UNORM target into its sRGB twin): the encoded bytes pass unchanged.
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {region.width, region.height, 1};
    vkCmdCopyImage(slot.Command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    // C: the target goes back to the layout the renderer's passes expect.
    Barrier(slot.Command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    // D: a released color swapchain image is in COLOR_ATTACHMENT_OPTIMAL (XR_KHR_vulkan_enable).
    Barrier(slot.Command, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(slot.Command);

    const uint64_t signalValue = gCopyValue + 1;
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount = 1;
    timeline.pWaitSemaphoreValues = &publication.RenderValue;
    timeline.signalSemaphoreValueCount = 1;
    timeline.pSignalSemaphoreValues = &signalValue;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.pNext = &timeline;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &gRenderTimeline;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.Command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &gCopyTimeline;
    VkResult submitted;
    {
        QueueLock lock("xr");
        submitted = vkQueueSubmit(g.Queue, 1, &submit, slot.Fence);
    }
    if (submitted != VK_SUCCESS) {
        XRERR("TRIAEVUM_XR_ERROR call=vkQueueSubmit(copy) result=%d", static_cast<int>(submitted));
        // The fence stays unsignaled after a failed submit; make the slot reusable.
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkDestroyFence(g.Device, slot.Fence, nullptr);
        vkCreateFence(g.Device, &fence, nullptr, &slot.Fence);
        ClearCopying(publication.Image);
        // A released colour image must be written and in COLOR_ATTACHMENT_OPTIMAL: keep this one
        // acquired and waited, and retry the copy next display frame (no new acquire or wait).
        if (submitted == VK_ERROR_DEVICE_LOST) {
            // Nothing will recover: stop showing the quad and end the run instead of looping.
            gQuadReleased = false;
            BeginExit("device_lost");
        }
        return;
    }
    gCopyValue = signalValue;
    static const bool releaseWait = EnvFlag("TRIAEVUM_XR_RELEASE_WAIT");
    if (releaseWait) vkWaitForFences(g.Device, 1, &slot.Fence, VK_TRUE, UINT64_MAX);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrResult released;
    {
        QueueLock lock("xr");
        released = xrReleaseSwapchainImage(gSwapchain, &release);
    }
    if (XR_FAILED(released))
        XRWARN("TRIAEVUM_XR_ERROR call=xrReleaseSwapchainImage result=%d", static_cast<int>(released));
    gPendingAcquire = -1;
    gPendingWaited = false;
    {
        std::lock_guard lock(gSink);
        Target& target = gTargets[publication.Image];
        target.LastCopyValue = signalValue;
        target.Copying = false;
        gShownSerial = publication.Serial;
        ++gStats.Copies;
    }
    gSinkChanged.notify_all();
    gQuadReleased = true;
    if (!gFirstImageLogged) {
        gFirstImageLogged = true;
        XRLOG("TRIAEVUM_XR_FIRST_IMAGE serial=%llu render_value=%llu xr_image=%u",
              static_cast<unsigned long long>(publication.Serial),
              static_cast<unsigned long long>(publication.RenderValue), index);
        NoteFirstImage();
    }
}

SinkStats TakeSinkStats() {
    std::lock_guard lock(gSink);
    const SinkStats stats = gStats;
    gStats = {};
    return stats;
}

void DestroyQuadSwapchain() {
    if (gSwapchain != XR_NULL_HANDLE) xrDestroySwapchain(gSwapchain);
    gSwapchain = XR_NULL_HANDLE;
    gSwapchainImages.clear();
    gQuadReleased = false;
}

void DestroyVulkanObjects() {
    if (!Requested()) return;
    auto& g = G();
    if (g.Device == VK_NULL_HANDLE) return;
    for (auto& slot : gCopySlots)
        if (slot.Fence != VK_NULL_HANDLE) vkDestroyFence(g.Device, slot.Fence, nullptr);
    gCopySlots.clear();
    if (gCommandPool != VK_NULL_HANDLE) vkDestroyCommandPool(g.Device, gCommandPool, nullptr);
    gCommandPool = VK_NULL_HANDLE;
    if (gRenderTimeline != VK_NULL_HANDLE) vkDestroySemaphore(g.Device, gRenderTimeline, nullptr);
    if (gCopyTimeline != VK_NULL_HANDLE) vkDestroySemaphore(g.Device, gCopyTimeline, nullptr);
    gRenderTimeline = gCopyTimeline = VK_NULL_HANDLE;
    XRLOG("TRIAEVUM_XR_EXIT stage=vulkan_objects_destroyed");
}

}  // namespace TriAevumQuestXr
