#include "oot3d_native_render_command_stream.h"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace Oot3dNativeGame {
namespace {

thread_local bool tInCommand = false;
thread_local bool tSynchronous = false;

struct CommandScope {
    explicit CommandScope(bool synchronous) {
        tInCommand = true;
        tSynchronous = synchronous;
    }
    ~CommandScope() {
        tInCommand = false;
        tSynchronous = false;
    }
};

// Adds the time a predicate wait blocked to `total` (nothing when the predicate already held).
template <typename Predicate>
void TimedWait(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, Predicate predicate,
               std::atomic<uint64_t>& total) {
    if (predicate()) return;
    const auto start = std::chrono::steady_clock::now();
    cv.wait(lock, predicate);
    total.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - start)
                                              .count()),
                    std::memory_order_relaxed);
}

} // namespace

uint64_t NativeRenderCommandStream::TakeSubmitterWaitNs() noexcept {
    return mSubmitterWaitNs.exchange(0);
}

uint64_t NativeRenderCommandStream::TakeWorkerIdleNs() noexcept {
    return mWorkerIdleNs.exchange(0);
}

NativeRenderCommandStream::NativeRenderCommandStream(Mode mode,
                                                     size_t maxFramesInFlight)
    : mMode(mode),
      mMaxFramesInFlight(maxFramesInFlight == 0U ? 1U : maxFramesInFlight) {}

NativeRenderCommandStream::~NativeRenderCommandStream() {
    StopWorker();
}

void NativeRenderCommandStream::SetMode(Mode mode) {
    Drain();
    if (mode == mMode) return;
    if (mode == Mode::Inline) StopWorker();
    mMode = mode;
}

bool NativeRenderCommandStream::InCommand() const noexcept {
    return tInCommand;
}

bool NativeRenderCommandStream::InSynchronousCommand() const noexcept {
    return tInCommand && tSynchronous;
}

uint64_t NativeRenderCommandStream::ExecutedCommands() const noexcept {
    std::lock_guard lock(mMutex);
    return mExecuted;
}

void NativeRenderCommandStream::Submit(Command command, bool endsFrame) {
    Enqueue(std::move(command), endsFrame, false);
}

void NativeRenderCommandStream::Enqueue(Command command, bool endsFrame,
                                        bool synchronous) {
    if (mMode == Mode::Inline) {
        {
            CommandScope scope(true);
            command();
        }
        std::lock_guard lock(mMutex);
        ++mSubmitted;
        ++mExecuted;
        return;
    }
    StartWorker();
    std::unique_lock lock(mMutex);
    RethrowPendingFailureLocked(lock);
    if (endsFrame) {
        TimedWait(mProgress, lock, [&] {
            return mFailure != nullptr || mFramesQueued < mMaxFramesInFlight;
        }, mSubmitterWaitNs);
        RethrowPendingFailureLocked(lock);
        ++mFramesQueued;
    }
    mQueue.push_back({std::move(command), endsFrame, synchronous});
    ++mSubmitted;
    mWork.notify_one();
}

void NativeRenderCommandStream::Run(Command command) {
    Drain();
    {
        CommandScope scope(true);
        command();
    }
    std::lock_guard lock(mMutex);
    ++mSubmitted;
    ++mExecuted;
}

void NativeRenderCommandStream::Drain() {
    if (mMode == Mode::Inline || !mWorker.joinable()) return;
    std::unique_lock lock(mMutex);
    TimedWait(mProgress, lock, [&] {
        return mFailure != nullptr || (mQueue.empty() && !mBusy);
    }, mSubmitterWaitNs);
    RethrowPendingFailureLocked(lock);
}

void NativeRenderCommandStream::WaitForFramesInFlight(size_t frames) {
    if (mMode == Mode::Inline || !mWorker.joinable()) return;
    std::unique_lock lock(mMutex);
    TimedWait(mProgress, lock, [&] {
        return mFailure != nullptr || mFramesQueued <= frames;
    }, mSubmitterWaitNs);
    RethrowPendingFailureLocked(lock);
}

void NativeRenderCommandStream::RethrowPendingFailureLocked(
    std::unique_lock<std::mutex>& lock) {
    if (mFailure == nullptr) return;
    // Report the failure once; the stream is empty and usable afterwards.
    auto failure = std::exchange(mFailure, nullptr);
    mQueue.clear();
    mFramesQueued = 0U;
    mExecuted = mSubmitted;
    lock.unlock();
    std::rethrow_exception(failure);
}

void NativeRenderCommandStream::StartWorker() {
    if (mWorker.joinable()) return;
    mStop = false;
    mWorker = std::thread([this] { WorkerLoop(); });
}

void NativeRenderCommandStream::StopWorker() {
    if (!mWorker.joinable()) return;
    {
        std::lock_guard lock(mMutex);
        mStop = true;
    }
    mWork.notify_all();
    mWorker.join();
}

void NativeRenderCommandStream::WorkerLoop() {
    std::unique_lock lock(mMutex);
    for (;;) {
        TimedWait(mWork, lock, [&] { return mStop || !mQueue.empty(); }, mWorkerIdleNs);
        if (mQueue.empty()) return;  // stopping with nothing left to run
        Entry entry = std::move(mQueue.front());
        mQueue.pop_front();
        mBusy = true;
        lock.unlock();
        std::exception_ptr failure;
        try {
            CommandScope scope(entry.Synchronous);
            entry.Work();
        } catch (...) {
            failure = std::current_exception();
        }
        lock.lock();
        mBusy = false;
        if (entry.EndsFrame && mFramesQueued != 0U) --mFramesQueued;
        if (failure != nullptr) {
            mFailure = failure;
            mQueue.clear();
            mFramesQueued = 0U;
        } else {
            ++mExecuted;
        }
        mProgress.notify_all();
    }
}

} // namespace Oot3dNativeGame
