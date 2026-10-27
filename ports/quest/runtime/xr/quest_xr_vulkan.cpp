// TriAevum Quest immersive mode: the quad swapchain, the renderer's private targets and their
// publication, the copy into the swapchain on the XR queue, the timelines and the XR queue lock.
// q3-spec §4.1, §5. In-world mode (inworld-spec §2.4, §3.3): three targets per frame slot (panel,
// eye L, eye R), stereo publications that carry their eye poses, the eye swapchain (arraySize 2) and
// the static fade swapchain. The HUD (W4, minimal): the panel of a stereo publication with panelValid
// is copied into its own swapchain in the same submit as the eyes. The in-frame copy (default on,
// TRIAEVUM_XR_FRAME_COPY=0: the XR queue copies as before): the XR thread holds an acquired, waited eye (and
// HUD) image ahead; the renderer records a pair's copies into its own frame submission (queue 0), and the XR
// thread releases the images once the render timeline reached that frame. The XR queue then only carries a
// release sync, so its copies can no longer queue behind the next frame's rendering.
#include "quest_xr_internal.h"
#include "quest_xr_input_logic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>

namespace TriAevumQuestXr {

namespace {

// ---- the XR queue lock
std::mutex gQueueMutex;
std::atomic<double> gLastLockLog{0.0};

// ---- swapchains and copy resources (created on the game thread before the XR thread starts)
struct CopySlot {
    VkCommandBuffer Command = VK_NULL_HANDLE;
    VkFence Fence = VK_NULL_HANDLE;
};
struct SwapchainState {
    const char* Name = "";
    XrSwapchain Handle = XR_NULL_HANDLE;
    VkExtent2D Extent{};
    uint32_t ArraySize = 1;
    std::vector<XrSwapchainImageVulkan2KHR> Images;
    std::vector<CopySlot> Slots;  // one per swapchain image
    // XR thread only: OpenXR waits the oldest acquired image and releases the oldest waited one, so
    // an image is never acquired while another is still unwaited (q3-spec §5.3).
    int PendingAcquire = -1;
    bool PendingWaited = false;
    std::atomic<bool> Released{false};  // at least one image released (the layer may reference it)
};
SwapchainState gQuad;  // Q3 screen / cinema picture
SwapchainState gEye;   // in-world: eye L -> layer 0, eye R -> layer 1
SwapchainState gFade;  // in-world: static 16-layer array of (0, 0, 0, k/15)
SwapchainState gHud;   // in-world: the HUD over transparent (premultiplied alpha), panel extent
constexpr uint32_t kFadeLevels = 16;
VkFormat gSwapchainFormat = VK_FORMAT_UNDEFINED;
VkFormat gTargetFormat = VK_FORMAT_R8G8B8A8_UNORM;
VkCommandPool gCommandPool = VK_NULL_HANDLE;
VkSemaphore gRenderTimeline = VK_NULL_HANDLE;
VkSemaphore gCopyTimeline = VK_NULL_HANDLE;
PFN_vkWaitSemaphores gWaitSemaphores = nullptr;
PFN_vkGetSemaphoreCounterValue gGetSemaphoreCounterValue = nullptr;

// ---- the sink, under gSink
struct Target {
    VkImage Image = VK_NULL_HANDLE;
    VkExtent2D Extent{};
    uint64_t LastCopyValue = 0;
    bool Copying = false;
};
enum class PublicationKind : uint8_t { Panel, Stereo };
struct Publication {
    int Slot = -1;  // frame slot (Q3: the target index, one per slot)
    uint64_t RenderValue = 0;
    uint64_t Serial = 0;
    PublicationKind Kind = PublicationKind::Panel;
    Fast::StereoViews Views{};
    bool PanelValid = false;
    double PublishedAt = 0.0;  // NowSeconds() at the publication (just after the frame's submit)
};
std::mutex gSink;
std::condition_variable gSinkChanged;
std::vector<Target> gTargets;
bool gViewTargets = false;  // in-world: three targets per slot (slot * 3 + view)
Publication gLatest;
uint64_t gSerial = 0;
uint64_t gShownSerial = 0;
SinkStats gStats;
// Shown-latency of stereo pairs (ticks from PaceTick to the first display tick), last 15.
std::array<uint32_t, 15> gLatency{};
size_t gLatencyCount = 0, gLatencyNext = 0;
PairStats gPairStats;
// In-frame copy: the held eye and HUD images (one acquired and waited ahead), and the claim tokens.
using HeldQueue = Logic::HeldImageQueue<Publication>;
HeldQueue gEyeHeld;
HeldQueue gHudHeld;
uint64_t gFrameCopyToken = 0;

// ---- XR thread only
uint64_t gCopyValue = 0;
std::atomic<uint64_t> gCopySubmitted{0};  // the copy timeline's last submitted value (renderer device waits)
bool gFirstImageLogged = false;
Fast::StereoViews gShownViews{};
bool gHasShownViews = false;
std::atomic<uint32_t> gPublishedRigEpoch{0};  // renderer writes, XR thread reads

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

void ClearCopying(const int* images, size_t count) {
    {
        std::lock_guard lock(gSink);
        for (size_t i = 0; i < count; ++i)
            if (images[i] >= 0 && static_cast<size_t>(images[i]) < gTargets.size()) gTargets[images[i]].Copying = false;
    }
    gSinkChanged.notify_all();
}

void Barrier(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
             VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
             uint32_t layers = 1) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCmdPipelineBarrier(command, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void CreateSwapchain(SwapchainState& swapchain, const char* name, VkExtent2D extent, uint32_t arraySize,
                     XrSwapchainCreateFlags flags = 0) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.createFlags = flags;
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                      XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = static_cast<int64_t>(gSwapchainFormat);
    info.sampleCount = 1;
    info.width = extent.width;
    info.height = extent.height;
    info.faceCount = 1;
    info.arraySize = arraySize;
    info.mipCount = 1;
    swapchain.Name = name;
    swapchain.Extent = extent;
    swapchain.ArraySize = arraySize;
    XrResult result = xrCreateSwapchain(G().Session, &info, &swapchain.Handle);
    if (XR_FAILED(result))
        throw std::runtime_error(std::string("xrCreateSwapchain(") + name + ") failed: XrResult " +
                                 std::to_string(static_cast<int>(result)));
    uint32_t images = 0;
    xrEnumerateSwapchainImages(swapchain.Handle, 0, &images, nullptr);
    swapchain.Images.assign(images, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    result = xrEnumerateSwapchainImages(swapchain.Handle, images, &images,
                                        reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.Images.data()));
    if (XR_FAILED(result) || images == 0U)
        throw std::runtime_error(std::string("xrEnumerateSwapchainImages(") + name + ") failed");
}

// Acquire (only when nothing is pending) and wait (100 ms, without the queue lock). The image index
// ready for writing, or -1 to try again next display frame.
int AcquireAndWait(SwapchainState& swapchain) {
    if (swapchain.PendingAcquire < 0) {
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult result;
        const double start = NowSeconds();
        {
            QueueLock lock("xr");
            result = xrAcquireSwapchainImage(swapchain.Handle, &acquire, &index);
        }
        PerfNote(PerfPhase::XrAcquire, (NowSeconds() - start) * 1000.0);
        if (XR_FAILED(result)) {
            XRWARN("TRIAEVUM_XR_ERROR call=xrAcquireSwapchainImage swapchain=%s result=%d", swapchain.Name,
                   static_cast<int>(result));
            return -1;
        }
        swapchain.PendingAcquire = static_cast<int>(index);
        swapchain.PendingWaited = false;
    }
    if (!swapchain.PendingWaited) {
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = 100'000'000;  // 100 ms
        const double start = NowSeconds();
        const XrResult waited = xrWaitSwapchainImage(swapchain.Handle, &wait);
        PerfNote(PerfPhase::XrWaitImage, (NowSeconds() - start) * 1000.0);
        if (waited == XR_TIMEOUT_EXPIRED || XR_FAILED(waited)) {
            if (waited != XR_TIMEOUT_EXPIRED)
                XRWARN("TRIAEVUM_XR_ERROR call=xrWaitSwapchainImage swapchain=%s result=%d", swapchain.Name,
                       static_cast<int>(waited));
            std::lock_guard lock(gSink);
            ++gStats.SwapchainWaitTimeouts;
            return -1;
        }
        swapchain.PendingWaited = true;
    }
    return swapchain.PendingAcquire;
}

// xrReleaseSwapchainImage of the oldest waited image (both acquisition modes).
void ReleaseOldest(SwapchainState& swapchain) {
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrResult released;
    const double start = NowSeconds();
    {
        QueueLock lock("xr");
        released = xrReleaseSwapchainImage(swapchain.Handle, &release);
    }
    PerfNote(PerfPhase::XrRelease, (NowSeconds() - start) * 1000.0);
    if (XR_FAILED(released))
        XRWARN("TRIAEVUM_XR_ERROR call=xrReleaseSwapchainImage swapchain=%s result=%d", swapchain.Name,
               static_cast<int>(released));
    swapchain.Released = true;
}

void ReleaseImage(SwapchainState& swapchain) {
    ReleaseOldest(swapchain);
    swapchain.PendingAcquire = -1;
    swapchain.PendingWaited = false;
}

// Submits `command` on queue 1 with `fence`; waits the render timeline at `renderValue` (0: none) and
// signals the copy timeline. False on a failed submit (the fence is replaced by a signalled one).
bool SubmitCopy(CopySlot& slot, uint64_t renderValue, uint64_t* signalled) {
    auto& g = G();
    const uint64_t signalValue = gCopyValue + 1;
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount = renderValue != 0U ? 1U : 0U;
    timeline.pWaitSemaphoreValues = &renderValue;
    timeline.signalSemaphoreValueCount = 1;
    timeline.pSignalSemaphoreValues = &signalValue;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.pNext = &timeline;
    submit.waitSemaphoreCount = renderValue != 0U ? 1U : 0U;
    submit.pWaitSemaphores = &gRenderTimeline;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot.Command;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &gCopyTimeline;
    VkResult submitted;
    const double start = NowSeconds();
    {
        QueueLock lock("xr");
        submitted = vkQueueSubmit(g.Queue, 1, &submit, slot.Fence);
    }
    PerfNote(PerfPhase::XrSubmit, (NowSeconds() - start) * 1000.0);
    if (submitted != VK_SUCCESS) {
        XRERR("TRIAEVUM_XR_ERROR call=vkQueueSubmit(copy) result=%d", static_cast<int>(submitted));
        // The fence stays unsignaled after a failed submit; make the slot reusable.
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkDestroyFence(g.Device, slot.Fence, nullptr);
        vkCreateFence(g.Device, &fence, nullptr, &slot.Fence);
        if (submitted == VK_ERROR_DEVICE_LOST) {
            // Nothing will recover: stop showing the layers and end the run instead of looping.
            gQuad.Released = false;
            gEye.Released = false;
            gHud.Released = false;
            BeginExit("device_lost");
        }
        return false;
    }
    gCopyValue = signalValue;
    gCopySubmitted.store(signalValue);
    *signalled = signalValue;
    return true;
}

VkCommandBuffer BeginSlot(CopySlot& slot) {
    auto& g = G();
    vkWaitForFences(g.Device, 1, &slot.Fence, VK_TRUE, UINT64_MAX);
    vkResetFences(g.Device, 1, &slot.Fence);
    vkResetCommandBuffer(slot.Command, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(slot.Command, &begin);
    return slot.Command;
}

void DestroySwapchain(SwapchainState& swapchain) {
    if (swapchain.Handle != XR_NULL_HANDLE) xrDestroySwapchain(swapchain.Handle);
    swapchain.Handle = XR_NULL_HANDLE;
    swapchain.Images.clear();
    swapchain.PendingAcquire = -1;
    swapchain.PendingWaited = false;
    swapchain.Released = false;
}

int TargetIndex(int slot, uint32_t view) {
    return gViewTargets ? slot * static_cast<int>(Fast::kXrViewCount) + static_cast<int>(view) : slot;
}

// ---- in-frame copy (XR thread): the held images

// One image ahead: acquire when the queue wants one, then wait 1 ms (retried every display frame, so a
// compositor still reading the image never stalls the frame loop).
void PrepareHeld(SwapchainState& swapchain, HeldQueue& queue) {
    if (swapchain.Handle == XR_NULL_HANDLE) return;
    bool acquire = false, wait = false;
    {
        std::lock_guard lock(gSink);
        acquire = queue.NeedsAcquire();
        wait = queue.HasUnwaited();
    }
    if (acquire) {
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrResult result;
        const double start = NowSeconds();
        {
            QueueLock lock("xr");
            result = xrAcquireSwapchainImage(swapchain.Handle, &info, &index);
        }
        PerfNote(PerfPhase::XrAcquire, (NowSeconds() - start) * 1000.0);
        if (XR_FAILED(result)) {
            size_t held = 0, capacity = 0;
            {
                std::lock_guard lock(gSink);
                held = queue.Size();
                // A runtime that allows fewer acquired images: hold only as many as it gave.
                if (result == XR_ERROR_CALL_ORDER_INVALID && held != 0U) queue.SetCapacity(held);
                capacity = queue.Capacity();
                ++gStats.HeldAcquireFailures;
            }
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 4)
                XRWARN("TRIAEVUM_XR_FRAME_COPY acquire_failed swapchain=%s result=%d held=%zu capacity=%zu",
                       swapchain.Name, static_cast<int>(result), held, capacity);
            return;
        }
        std::lock_guard lock(gSink);
        queue.Acquired(static_cast<int>(index));
        wait = true;
    }
    if (!wait) return;
    XrSwapchainImageWaitInfo info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    info.timeout = 1000000;  // 1 ms
    const double start = NowSeconds();
    const XrResult waited = xrWaitSwapchainImage(swapchain.Handle, &info);
    PerfNote(PerfPhase::XrWaitImage, (NowSeconds() - start) * 1000.0);
    std::lock_guard lock(gSink);
    if (waited == XR_SUCCESS) {
        queue.Waited();
    } else {
        ++gStats.HeldWaitTimeouts;
        if (waited != XR_TIMEOUT_EXPIRED)
            XRWARN("TRIAEVUM_XR_ERROR call=xrWaitSwapchainImage swapchain=%s result=%d", swapchain.Name,
                   static_cast<int>(waited));
    }
}

// The XR queue's release sync: an empty submit that waits the render timeline at `value` (already
// reached on the host), so the runtime's work on the binding queue is ordered after the frame's writes.
bool SubmitReleaseSync(uint64_t value) {
    auto& g = G();
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount = 1;
    timeline.pWaitSemaphoreValues = &value;
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.pNext = &timeline;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &gRenderTimeline;
    submit.pWaitDstStageMask = &stage;
    VkResult result;
    const double start = NowSeconds();
    {
        QueueLock lock("xr");
        result = vkQueueSubmit(g.Queue, 1, &submit, VK_NULL_HANDLE);
    }
    PerfNote(PerfPhase::XrSubmit, (NowSeconds() - start) * 1000.0);
    if (result == VK_SUCCESS) return true;
    XRERR("TRIAEVUM_XR_ERROR call=vkQueueSubmit(release_sync) result=%d", static_cast<int>(result));
    if (result == VK_ERROR_DEVICE_LOST) {
        gQuad.Released = false;
        gEye.Released = false;
        gHud.Released = false;
        BeginExit("device_lost");
    }
    return false;
}

void NoteShownLatencyLocked(const Publication& publication, uint64_t tick) {
    // Shown latency: ticks from the renderer's pace tick to this first display.
    const uint64_t pace = publication.Views.PaceTick;
    const uint32_t latency = tick >= pace ? static_cast<uint32_t>(std::min<uint64_t>(tick - pace, 64)) : 0U;
    gLatency[gLatencyNext] = latency;
    gLatencyNext = (gLatencyNext + 1) % gLatency.size();
    gLatencyCount = std::min(gLatencyCount + 1, gLatency.size());
    ++gPairStats.Pairs;
    if (publication.Views.TargetTick != 0U && tick > publication.Views.TargetTick) ++gPairStats.LatePairs;
}

void NoteFirstImageOnce(const Publication& publication, int index) {
    if (gFirstImageLogged) return;
    gFirstImageLogged = true;
    XRLOG("TRIAEVUM_XR_FIRST_IMAGE serial=%llu render_value=%llu xr_image=%d kind=%s",
          static_cast<unsigned long long>(publication.Serial),
          static_cast<unsigned long long>(publication.RenderValue), index,
          publication.Kind == PublicationKind::Stereo ? "stereo" : "panel");
    NoteFirstImage();
}

// Releases every held image whose frame is GPU-complete, oldest first, after one release sync; the last
// released pair is the one on show, with its own poses.
void ServiceHeld(uint64_t tick) {
    auto& g = G();
    std::vector<HeldQueue::Entry> eyes, huds;
    {
        std::lock_guard lock(gSink);
        uint64_t completed = 0;
        if (gGetSemaphoreCounterValue(g.Device, gRenderTimeline, &completed) != VK_SUCCESS) completed = 0;
        eyes = gEyeHeld.TakeReleasable(completed);
        huds = gHudHeld.TakeReleasable(completed);
    }
    if (eyes.empty() && huds.empty()) return;
    uint64_t sync = 0;
    for (const auto& e : eyes) sync = std::max(sync, e.RenderValue);
    for (const auto& e : huds) sync = std::max(sync, e.RenderValue);
    if (sync != 0U) (void)SubmitReleaseSync(sync);
    for (size_t i = 0; i < eyes.size(); ++i) ReleaseOldest(gEye);
    for (size_t i = 0; i < huds.size(); ++i) ReleaseOldest(gHud);
    const HeldQueue::Entry* shown = nullptr;
    for (auto it = eyes.rbegin(); it != eyes.rend(); ++it)
        if (!it->Orphan) {
            shown = &*it;
            break;
        }
    const double now = NowSeconds();
    {
        std::lock_guard lock(gSink);
        for (const auto& e : eyes) {
            if (e.Orphan) {
                ++gStats.FrameCopyOrphans;
            } else if (&e != shown) {
                ++gStats.PublicationsDropped;  // released in the same display frame as a newer pair: never shown
            }
        }
        for (const auto& e : huds)
            if (!e.Orphan) ++gStats.HudCopies;
        if (shown != nullptr) {
            ++gStats.Copies;
            ++gStats.StereoCopies;
            gShownSerial = std::max(gShownSerial, shown->Data.Serial);
            NoteShownLatencyLocked(shown->Data, tick);
        }
    }
    gSinkChanged.notify_all();
    if (shown == nullptr) return;
    gShownViews = shown->Data.Views;
    gHasShownViews = true;
    if (shown->Data.PublishedAt > 0.0) PerfNote(PerfPhase::PairShown, (now - shown->Data.PublishedAt) * 1000.0);
    NoteFirstImageOnce(shown->Data, shown->Index);
    static bool hudLogged = false;
    if (!hudLogged && !huds.empty()) {
        hudLogged = true;
        XRLOG("TRIAEVUM_XR_HUD first_copy serial=%llu xr_image=%d w=%u h=%u in_frame=1",
              static_cast<unsigned long long>(shown->Data.Serial), huds.back().Index, gHud.Extent.width,
              gHud.Extent.height);
    }
}

}  // namespace

QueueLock::QueueLock(const char* holder) : mHolder(holder) {
    const auto before = std::chrono::steady_clock::now();
    gQueueMutex.lock();
    mStart = std::chrono::steady_clock::now();
    const bool renderer = holder[0] == 'r';
    PerfNote(renderer ? PerfPhase::LockWaitRenderer : PerfPhase::LockWaitXr,
             std::chrono::duration<double, std::milli>(mStart - before).count());
}

QueueLock::~QueueLock() {
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mStart).count();
    gQueueMutex.unlock();
    PerfNote(mHolder[0] == 'r' ? PerfPhase::LockHoldRenderer : PerfPhase::LockHoldXr, ms);
    if (ms > 5.0) {
        const double now = NowSeconds();
        double last = gLastLockLog.load();
        if (now - last >= 2.0 && gLastLockLog.compare_exchange_strong(last, now))
            XRLOG("TRIAEVUM_XR_QUEUE_LOCK holder=%s ms=%.1f", mHolder, ms);
    }
}

VkResult DeviceWaitIdleLocked(VkDevice device) {
    if (!Requested()) return vkDeviceWaitIdle(device);  // the flat panel: no XR queue user
    auto& g = G();
    // Before the session (no XR thread) and after StopSession (joined, queue 1 idle) nothing else uses
    // queue 1.
    if (g.Queue == VK_NULL_HANDLE || g.SessionStopped.load()) return vkDeviceWaitIdle(device);
    // Default: the renderer never touches the XR queue (so it never waits behind a blocking xrBeginFrame
    // under the queue lock): its own queue idle, then every XR-queue copy that reads its targets.
    // TRIAEVUM_XR_RENDERER_WAIT=device restores vkDeviceWaitIdle under the lock.
    static const bool deviceWait = [] {
        const char* value = std::getenv("TRIAEVUM_XR_RENDERER_WAIT");
        const bool device = value != nullptr && std::string(value) == "device";
        XRLOG("TRIAEVUM_XR_RENDERER_WAIT mode=%s", device ? "device" : "queue");
        return device;
    }();
    if (deviceWait) {
        QueueLock lock("renderer");
        return vkDeviceWaitIdle(device);
    }
    VkQueue rendererQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, g.QueueFamily, 0, &rendererQueue);
    const VkResult result = rendererQueue != VK_NULL_HANDLE ? vkQueueWaitIdle(rendererQueue) : vkDeviceWaitIdle(device);
    WaitCopyTimeline(gCopySubmitted.load(), "renderer_wait");
    return result;
}

VkFormat TargetFormat() { return gTargetFormat; }
XrSwapchain QuadSwapchain() { return gQuad.Handle; }
VkExtent2D QuadExtent() { return gQuad.Extent; }
bool QuadReleased() { return gQuad.Released.load(); }
XrSwapchain EyeSwapchain() { return gEye.Handle; }
VkExtent2D EyeSwapchainExtent() { return gEye.Extent; }
bool EyeReleased() { return gEye.Released.load(); }
XrSwapchain FadeSwapchain() { return gFade.Handle; }
uint32_t FadeLevels() { return kFadeLevels; }
XrSwapchain HudSwapchain() { return gHud.Handle; }
VkExtent2D HudSwapchainExtent() { return gHud.Extent; }
bool HudReleased() { return gHud.Handle != XR_NULL_HANDLE && gHud.Released.load(); }
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
    CreateSwapchain(gQuad, "quad", PanelExtent(), 1);
    std::string list;
    for (int64_t format : formats) list += (list.empty() ? "" : ",") + std::to_string(format);
    XRLOG("TRIAEVUM_XR_SWAPCHAIN w=%u h=%u format=%d target_format=%d images=%zu offered=%s", gQuad.Extent.width,
          gQuad.Extent.height, static_cast<int>(gSwapchainFormat), static_cast<int>(gTargetFormat),
          gQuad.Images.size(), list.c_str());
}

void CreateWorldSwapchains() {
    // The eye swapchain (§3.3): Q3's format rule, EyeExtent(), one array image with a layer per eye.
    const VkExtent2D eye = EyeExtent();
    CreateSwapchain(gEye, "eye", eye, 2);
    XRLOG("TRIAEVUM_XR_SWAPCHAIN eye=%ux%u array=2 format=%d target_format=%d images=%zu", eye.width, eye.height,
          static_cast<int>(gSwapchainFormat), static_cast<int>(gTargetFormat), gEye.Images.size());
    // The fade: a static 4x4 array, layer k = (0, 0, 0, k / 15); a fade picks a layer, nothing is
    // rendered per frame (cleared once, on the XR thread, by PrepareFadeSwapchain).
    CreateSwapchain(gFade, "fade", {4, 4}, kFadeLevels, XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT);
    XRLOG("TRIAEVUM_XR_SWAPCHAIN fade=4x4 array=%u images=%zu", kFadeLevels, gFade.Images.size());
    // The HUD: the panel's extent and format (the sRGB swapchain receives the UNORM panel's encoded
    // bytes unchanged, like the quad). The panel holds premultiplied colour in the encoded space.
    if (HudEnabled()) {
        CreateSwapchain(gHud, "hud", PanelExtent(), 1);
        XRLOG("TRIAEVUM_XR_SWAPCHAIN hud=%ux%u format=%d images=%zu alpha=premultiplied", gHud.Extent.width,
              gHud.Extent.height, static_cast<int>(gSwapchainFormat), gHud.Images.size());
    } else {
        XRLOG("TRIAEVUM_XR_SWAPCHAIN hud=off (TRIAEVUM_XR_HUD=0)");
    }
}

bool PrepareFadeSwapchain() {
    if (gFade.Handle == XR_NULL_HANDLE) return false;
    if (gFade.Released.load()) return true;
    const int index = AcquireAndWait(gFade);
    if (index < 0) return false;
    CopySlot& slot = gFade.Slots[static_cast<size_t>(index)];
    VkCommandBuffer command = BeginSlot(slot);
    const VkImage image = gFade.Images[static_cast<size_t>(index)].image;
    Barrier(command, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            kFadeLevels);
    for (uint32_t level = 0; level < kFadeLevels; ++level) {
        VkClearColorValue color{};
        color.float32[3] = static_cast<float>(level) / static_cast<float>(kFadeLevels - 1);  // black, premultiplied
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, level, 1};
        vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    }
    Barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            kFadeLevels);
    vkEndCommandBuffer(command);
    uint64_t signalled = 0;
    if (!SubmitCopy(slot, 0, &signalled)) return false;
    ReleaseImage(gFade);
    XRLOG("TRIAEVUM_XR_FADE ready levels=%u", kFadeLevels);
    return true;
}

void CreateCopyResources() {
    auto& g = G();
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = g.QueueFamily;
    Require(vkCreateCommandPool(g.Device, &pool, nullptr, &gCommandPool), "vkCreateCommandPool(xr)");
    for (SwapchainState* swapchain : {&gQuad, &gEye, &gFade, &gHud}) {
        swapchain->Slots.assign(swapchain->Images.size(), {});
        for (auto& slot : swapchain->Slots) {
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocate.commandPool = gCommandPool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 1;
            Require(vkAllocateCommandBuffers(g.Device, &allocate, &slot.Command), "vkAllocateCommandBuffers(xr)");
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            Require(vkCreateFence(g.Device, &fence, nullptr, &slot.Fence), "vkCreateFence(xr)");
        }
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
        for (uint32_t index = 0; index < count; ++index) {
            gTargets[index].Image = images[index];
            gTargets[index].Extent = extent;
        }
        gViewTargets = false;
        gLatest = {};
    }
    XRLOG("TRIAEVUM_XR_TARGETS count=%u w=%u h=%u format=%d", count, extent.width, extent.height,
          static_cast<int>(gTargetFormat));
}

void AttachViewTargets(const VkImage* images, uint32_t slots, const VkExtent2D extents[3]) {
    {
        std::lock_guard lock(gSink);
        gTargets.assign(static_cast<size_t>(slots) * Fast::kXrViewCount, {});
        for (uint32_t index = 0; index < gTargets.size(); ++index) {
            gTargets[index].Image = images[index];
            gTargets[index].Extent = extents[index % Fast::kXrViewCount];
        }
        gViewTargets = true;
        gLatest = {};
    }
    XRLOG("TRIAEVUM_XR_TARGETS slots=%u views=3 panel=%ux%u eye=%ux%u format=%d", slots, extents[0].width,
          extents[0].height, extents[1].width, extents[1].height, static_cast<int>(gTargetFormat));
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
        const int slot = gViewTargets ? static_cast<int>(image / Fast::kXrViewCount) : static_cast<int>(image);
        if (gLatest.Slot == slot) {
            // The renderer reuses the slot before the XR thread showed it: drop that frame.
            gLatest.Slot = -1;
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
    if (image >= gTargets.size() || gViewTargets) return;
    gLatest = {};
    gLatest.Slot = static_cast<int>(image);
    gLatest.RenderValue = renderValue;
    gLatest.Serial = ++gSerial;
    ++gStats.Publications;
}

void PublishPanel(uint32_t slot, uint64_t renderValue) {
    std::lock_guard lock(gSink);
    if (!gViewTargets || static_cast<size_t>(slot) * Fast::kXrViewCount >= gTargets.size()) return;
    gLatest = {};
    gLatest.Slot = static_cast<int>(slot);
    gLatest.RenderValue = renderValue;
    gLatest.Serial = ++gSerial;
    gLatest.Kind = PublicationKind::Panel;
    ++gStats.Publications;
}

void PublishStereo(uint32_t slot, uint64_t renderValue, const Fast::StereoViews& views, bool panelValid,
                   uint64_t frameCopyToken) {
    std::lock_guard lock(gSink);
    Publication publication;
    publication.Slot = static_cast<int>(slot);
    publication.RenderValue = renderValue;
    publication.Kind = PublicationKind::Stereo;
    publication.Views = views;
    // The HUD swapchain belongs to the XR thread: CopyLatest checks gHud.Handle there, not here.
    publication.PanelValid = panelValid;
    publication.PublishedAt = NowSeconds();
    if (!gViewTargets || static_cast<size_t>(slot) * Fast::kXrViewCount >= gTargets.size()) {
        if (frameCopyToken != 0U) {
            // Never left claimed: the images go with the next pair.
            gEyeHeld.Abandon(frameCopyToken, renderValue);
            gHudHeld.Abandon(frameCopyToken, renderValue);
        }
        return;
    }
    publication.Serial = ++gSerial;
    if (views.RigEpoch > gPublishedRigEpoch.load()) gPublishedRigEpoch.store(views.RigEpoch);
    ++gStats.Publications;
    ++gStats.StereoPublications;
    if (publication.PanelValid) ++gStats.HudPublications;
    if (frameCopyToken != 0U) {
        // The pair is already in its swapchain images (this frame's submission wrote them): the XR thread
        // releases them once the render timeline reaches renderValue. An unshown XR-queue publication
        // before it is superseded.
        gEyeHeld.Commit(frameCopyToken, renderValue, publication);
        gHudHeld.Commit(frameCopyToken, renderValue, publication);
        ++gStats.FrameCopies;
        if (gLatest.Slot >= 0 && gLatest.Serial != gShownSerial) ++gStats.PublicationsDropped;
        gLatest = {};
        return;
    }
    gLatest = publication;
}

void AbandonFrameCopy(uint64_t token, uint64_t renderValue) {
    if (token == 0U) return;
    std::lock_guard lock(gSink);
    gEyeHeld.Abandon(token, renderValue);
    gHudHeld.Abandon(token, renderValue);
}

bool FrameCopyEnabled() {
    static const bool enabled = [] {
        const bool on = WorldRequested() && EnvFlag("TRIAEVUM_XR_FRAME_COPY", true);
        XRLOG("TRIAEVUM_XR_FRAME_COPY enabled=%d", on ? 1 : 0);
        return on;
    }();
    return enabled;
}

uint64_t RecordFrameCopy(VkCommandBuffer command, uint32_t slot, bool hud) {
    if (!FrameCopyEnabled() || command == VK_NULL_HANDLE) return 0;
    uint64_t token = 0;
    std::array<VkImage, 3> sources{};
    std::array<VkExtent2D, 3> extents{};
    VkImage eyeImage = VK_NULL_HANDLE, hudImage = VK_NULL_HANDLE;
    {
        std::lock_guard lock(gSink);
        const size_t base = static_cast<size_t>(slot) * Fast::kXrViewCount;
        if (!gViewTargets || gEye.Handle == XR_NULL_HANDLE || base + Fast::kXrViewCount > gTargets.size()) return 0;
        token = ++gFrameCopyToken;
        const int eye = gEyeHeld.Claim(token);
        if (eye < 0 || static_cast<size_t>(eye) >= gEye.Images.size()) {
            ++gStats.FrameCopyMisses;  // no image held ahead: the pair goes the XR queue's way
            return 0;
        }
        int hudIndex = -1;
        if (hud && gHud.Handle != XR_NULL_HANDLE) {
            hudIndex = gHudHeld.Claim(token);
            if (hudIndex < 0 || static_cast<size_t>(hudIndex) >= gHud.Images.size()) {
                hudIndex = -1;
                ++gStats.HudSkips;  // the HUD keeps its last image
            }
        }
        eyeImage = gEye.Images[static_cast<size_t>(eye)].image;
        if (hudIndex >= 0) hudImage = gHud.Images[static_cast<size_t>(hudIndex)].image;
        const std::array<uint32_t, 3> views{Fast::kXrViewEyeLeft, Fast::kXrViewEyeRight, Fast::kXrViewPanel};
        for (size_t i = 0; i < 3; ++i) {
            const Target& target = gTargets[base + views[i]];
            const VkExtent2D destination = i < 2 ? gEye.Extent : gHud.Extent;
            sources[i] = target.Image;
            extents[i] = {std::min(target.Extent.width, destination.width),
                          std::min(target.Extent.height, destination.height)};
        }
    }
    const size_t total = hudImage != VK_NULL_HANDLE ? 3U : 2U;
    // A: the targets (PRESENT_SRC after this frame's scanouts and overlay passes) become copy sources once
    //    every earlier command of the frame wrote them.
    for (size_t i = 0; i < total; ++i)
        Barrier(command, sources[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
    // B: the swapchain images' previous contents are discarded (both eye layers).
    Barrier(command, eyeImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            gEye.ArraySize);
    if (hudImage != VK_NULL_HANDLE)
        Barrier(command, hudImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // Same-size formats (UNORM target into its sRGB twin): the encoded bytes pass unchanged, alpha too.
    for (size_t i = 0; i < total; ++i) {
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, i < 2 ? static_cast<uint32_t>(i) : 0U, 1};
        copy.extent = {extents[i].width, extents[i].height, 1};
        vkCmdCopyImage(command, sources[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, i < 2 ? eyeImage : hudImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    // C: the targets go back to the layout the renderer's passes expect (the next frame of this slot).
    for (size_t i = 0; i < total; ++i)
        Barrier(command, sources[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    // D: a released color swapchain image is in COLOR_ATTACHMENT_OPTIMAL (XR_KHR_vulkan_enable).
    Barrier(command, eyeImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            gEye.ArraySize);
    if (hudImage != VK_NULL_HANDLE)
        Barrier(command, hudImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    static bool logged = false;
    if (!logged) {
        logged = true;
        XRLOG("TRIAEVUM_XR_FRAME_COPY first slot=%u hud=%d eye=%ux%u", slot, hudImage != VK_NULL_HANDLE ? 1 : 0,
              extents[0].width, extents[0].height);
    }
    return token;
}

namespace {

// The XR queue's copy of the latest publication (the Q3 quad, the cinema panel, and a pair the renderer
// could not copy in its frame), once it is GPU-complete. With the in-frame copy the eye and HUD images
// come from the held queues and are released by ServiceHeld, in acquire order.
void CopyQueued(uint64_t tick, bool held) {
    auto& g = G();
    Publication publication;
    // Sources [0, count) go to `swapchain` (the eyes, or the panel); for a stereo pair with panelValid
    // the slot's panel follows at index count (= 2) and goes to the HUD swapchain.
    std::array<int, 3> sources{-1, -1, -1};
    std::array<VkImage, 3> images{};
    std::array<VkExtent2D, 3> extents{};
    size_t count = 0;
    bool hud = false;
    SwapchainState* swapchain = &gQuad;
    uint64_t token = 0;
    int heldIndex = -1, heldHudIndex = -1;
    {
        std::lock_guard lock(gSink);
        if (gTargets.empty() || gLatest.Slot < 0 || gLatest.Serial == gShownSerial) {
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
        if (publication.Kind == PublicationKind::Stereo) {
            if (gEye.Handle == XR_NULL_HANDLE) {
                gShownSerial = publication.Serial;  // nothing can show it
                return;
            }
            sources = {TargetIndex(publication.Slot, Fast::kXrViewEyeLeft),
                       TargetIndex(publication.Slot, Fast::kXrViewEyeRight), -1};
            count = 2;
            swapchain = &gEye;
            hud = publication.PanelValid && gHud.Handle != XR_NULL_HANDLE;
            if (hud) sources[2] = TargetIndex(publication.Slot, Fast::kXrViewPanel);
            if (held) {
                // The held image ahead (none yet: PrepareHeld acquires it; retried next display frame).
                token = ++gFrameCopyToken;
                heldIndex = gEyeHeld.Claim(token);
                if (heldIndex < 0) {
                    ++gStats.HeldImageWaits;
                    return;
                }
                if (hud) {
                    heldHudIndex = gHudHeld.Claim(token);
                    if (heldHudIndex < 0) {
                        hud = false;
                        sources[2] = -1;
                        ++gStats.HudSkips;
                    }
                }
            }
        } else {
            sources[0] = TargetIndex(publication.Slot, Fast::kXrViewPanel);
            count = 1;
        }
        const size_t total = count + (hud ? 1U : 0U);
        for (size_t i = 0; i < total; ++i) {
            if (sources[i] < 0 || static_cast<size_t>(sources[i]) >= gTargets.size()) {
                for (size_t j = 0; j < i; ++j) gTargets[sources[j]].Copying = false;
                gShownSerial = publication.Serial;
                if (token != 0U) {
                    gEyeHeld.Abandon(token, 0);
                    gHudHeld.Abandon(token, 0);
                }
                return;
            }
            Target& target = gTargets[sources[i]];
            target.Copying = true;
            const VkExtent2D destination = i < count ? swapchain->Extent : gHud.Extent;
            images[i] = target.Image;
            extents[i] = {std::min(target.Extent.width, destination.width),
                          std::min(target.Extent.height, destination.height)};
        }
    }
    size_t total = count + (hud ? 1U : 0U);
    int index = heldIndex;
    int hudIndex = heldHudIndex;
    if (token == 0U) {
        index = AcquireAndWait(*swapchain);
        if (index < 0) {
            ClearCopying(sources.data(), total);
            return;
        }
        if (hud) {
            hudIndex = AcquireAndWait(gHud);
            if (hudIndex < 0) {
                // The eyes go on without it; the HUD keeps its last image (an acquired one stays pending).
                ClearCopying(&sources[count], 1);
                hud = false;
                total = count;
                std::lock_guard lock(gSink);
                ++gStats.HudSkips;
            }
        }
    }
    CopySlot& slot = swapchain->Slots[static_cast<size_t>(index)];
    VkCommandBuffer command = BeginSlot(slot);
    const VkImage destination = swapchain->Images[static_cast<size_t>(index)].image;
    const VkImage hudDestination = hud ? gHud.Images[static_cast<size_t>(hudIndex)].image : VK_NULL_HANDLE;
    // A: the renderer's targets (left in PRESENT_SRC by their scanouts and overlay passes) become copy
    //    sources; the render-timeline wait (TRANSFER stage) orders them after the frame.
    for (size_t i = 0; i < total; ++i)
        Barrier(command, images[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // B: the swapchain images' previous contents are discarded (every layer).
    Barrier(command, destination, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            swapchain->ArraySize);
    if (hud)
        Barrier(command, hudDestination, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // Same-size formats (UNORM target into its sRGB twin): the encoded bytes pass unchanged, alpha too.
    // Eye i goes to layer i; the panel of a pair to the HUD image.
    for (size_t i = 0; i < total; ++i) {
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, i < count ? static_cast<uint32_t>(i) : 0U, 1};
        copy.extent = {extents[i].width, extents[i].height, 1};
        vkCmdCopyImage(command, images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       i < count ? destination : hudDestination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    // C: the targets go back to the layout the renderer's passes expect.
    for (size_t i = 0; i < total; ++i)
        Barrier(command, images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    // D: a released color swapchain image is in COLOR_ATTACHMENT_OPTIMAL (XR_KHR_vulkan_enable).
    Barrier(command, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            swapchain->ArraySize);
    if (hud)
        Barrier(command, hudDestination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(command);

    uint64_t signalValue = 0;
    if (!SubmitCopy(slot, publication.RenderValue, &signalValue)) {
        ClearCopying(sources.data(), total);
        if (token != 0U) {
            // The held images were claimed but never written: orphans, released with the next pair.
            std::lock_guard lock(gSink);
            gEyeHeld.Abandon(token, 0);
            gHudHeld.Abandon(token, 0);
        }
        // A released colour image must be written and in COLOR_ATTACHMENT_OPTIMAL: keep these acquired
        // and waited, and retry the copy next display frame (no new acquire or wait).
        return;
    }
    if (token != 0U) {
        // In acquire order behind any pair of the renderer's own frames: ServiceHeld releases it.
        std::lock_guard lock(gSink);
        for (size_t i = 0; i < total; ++i) {
            Target& target = gTargets[sources[i]];
            target.LastCopyValue = signalValue;
            target.Copying = false;
        }
        gEyeHeld.Commit(token, 0, publication, false);
        if (hud) gHudHeld.Commit(token, 0, publication, false);
        gShownSerial = publication.Serial;
        ++gStats.QueueCopies;
        gSinkChanged.notify_all();
        return;
    }
    static const bool releaseWait = EnvFlag("TRIAEVUM_XR_RELEASE_WAIT");
    if (releaseWait) vkWaitForFences(g.Device, 1, &slot.Fence, VK_TRUE, UINT64_MAX);
    ReleaseImage(*swapchain);
    if (hud) ReleaseImage(gHud);
    {
        std::lock_guard lock(gSink);
        for (size_t i = 0; i < total; ++i) {
            Target& target = gTargets[sources[i]];
            target.LastCopyValue = signalValue;
            target.Copying = false;
        }
        gShownSerial = publication.Serial;
        ++gStats.Copies;
        if (hud) ++gStats.HudCopies;
        if (publication.Kind == PublicationKind::Stereo) {
            ++gStats.StereoCopies;
            NoteShownLatencyLocked(publication, tick);
        }
    }
    gSinkChanged.notify_all();
    if (publication.Kind == PublicationKind::Stereo) {
        gShownViews = publication.Views;
        gHasShownViews = true;
        if (publication.PublishedAt > 0.0)
            PerfNote(PerfPhase::PairShown, (NowSeconds() - publication.PublishedAt) * 1000.0);
    }
    NoteFirstImageOnce(publication, index);
    static bool hudLogged = false;
    if (hud && !hudLogged) {
        hudLogged = true;
        XRLOG("TRIAEVUM_XR_HUD first_copy serial=%llu xr_image=%d w=%u h=%u",
              static_cast<unsigned long long>(publication.Serial), hudIndex, extents[count].width,
              extents[count].height);
    }
}

}  // namespace

void CopyLatest(uint64_t tick) {
    // In-frame copy: release what the renderer's frames wrote, keep an image ahead, copy what it could not,
    // and release that too (in acquire order).
    const bool held = FrameCopyEnabled();
    if (held) {
        ServiceHeld(tick);
        PrepareHeld(gEye, gEyeHeld);
        PrepareHeld(gHud, gHudHeld);
    }
    CopyQueued(tick, held);
    if (held) {
        ServiceHeld(tick);
        PrepareHeld(gEye, gEyeHeld);
        PrepareHeld(gHud, gHudHeld);
    }
}

uint32_t LatestPublishedRigEpoch() { return gPublishedRigEpoch.load(); }

bool ShownViews(Fast::StereoViews* views) {
    if (!gHasShownViews) return false;
    *views = gShownViews;
    return true;
}

uint32_t ShownLatencyTicks() {
    std::lock_guard lock(gSink);
    if (gLatencyCount == 0) return 2;  // §3.1 default until measured
    std::array<uint32_t, 15> sorted{};
    std::copy(gLatency.begin(), gLatency.begin() + static_cast<std::ptrdiff_t>(gLatencyCount), sorted.begin());
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(gLatencyCount));
    return std::clamp<uint32_t>(sorted[gLatencyCount / 2], 1U, 6U);
}

PairStats TakePairStats() {
    std::lock_guard lock(gSink);
    PairStats stats = gPairStats;
    gPairStats = {};
    if (gLatencyCount != 0) {
        std::array<uint32_t, 15> sorted{};
        std::copy(gLatency.begin(), gLatency.begin() + static_cast<std::ptrdiff_t>(gLatencyCount), sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(gLatencyCount));
        stats.LatencyTicks = sorted[gLatencyCount / 2];
    } else {
        stats.LatencyTicks = 2;
    }
    return stats;
}

SinkStats TakeSinkStats() {
    std::lock_guard lock(gSink);
    const SinkStats stats = gStats;
    gStats = {};
    return stats;
}

void DestroyQuadSwapchain() {
    {
        std::lock_guard lock(gSink);
        gEyeHeld.Clear();
        gHudHeld.Clear();
    }
    DestroySwapchain(gQuad);
    DestroySwapchain(gEye);
    DestroySwapchain(gFade);
    DestroySwapchain(gHud);
    gHasShownViews = false;
}

void DestroyVulkanObjects() {
    if (!Requested()) return;
    auto& g = G();
    if (g.Device == VK_NULL_HANDLE) return;
    for (SwapchainState* swapchain : {&gQuad, &gEye, &gFade, &gHud}) {
        for (auto& slot : swapchain->Slots)
            if (slot.Fence != VK_NULL_HANDLE) vkDestroyFence(g.Device, slot.Fence, nullptr);
        swapchain->Slots.clear();
    }
    if (gCommandPool != VK_NULL_HANDLE) vkDestroyCommandPool(g.Device, gCommandPool, nullptr);
    gCommandPool = VK_NULL_HANDLE;
    if (gRenderTimeline != VK_NULL_HANDLE) vkDestroySemaphore(g.Device, gRenderTimeline, nullptr);
    if (gCopyTimeline != VK_NULL_HANDLE) vkDestroySemaphore(g.Device, gCopyTimeline, nullptr);
    gRenderTimeline = gCopyTimeline = VK_NULL_HANDLE;
    XRLOG("TRIAEVUM_XR_EXIT stage=vulkan_objects_destroyed");
}

}  // namespace TriAevumQuestXr
