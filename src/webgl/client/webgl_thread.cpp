#include "webgl/client/webgl_thread.h"

#include "webgl/client/webgl_channel.h"

#include <algorithm>
#include <cassert>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace bro::webgl {

namespace {

// "bro-webgl" in debuggers, profilers and top.
void nameThisThread() {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "bro-webgl");
#elif defined(__APPLE__)
    pthread_setname_np("bro-webgl");
#elif defined(_WIN32)
    // SetThreadDescription is Windows 10 1607 and later: looked up, not linked.
    using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
        if (auto fn = reinterpret_cast<SetDescription>(
                reinterpret_cast<void*>(GetProcAddress(kernel, "SetThreadDescription"))))
            fn(GetCurrentThread(), L"bro-webgl");
#endif
}

}  // namespace

WebGLThread::WebGLThread() : thread_([this] { run(); }) {}

WebGLThread::~WebGLThread() {
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        stop_ = true;
    }
    controlPending_.store(true, std::memory_order_release);
    wake();
    thread_.join();
}

void WebGLThread::add(WebGLChannel& channel) {
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        adds_.push_back(&channel);
    }
    controlPending_.store(true, std::memory_order_release);
    wake();
}

void WebGLThread::awaitDestroyed(WebGLChannel& channel) {
    wake();
    std::unique_lock<std::mutex> lock(controlMutex_);
    destroyedCv_.wait(lock, [&] { return channel.destroyed_; });
}

bool WebGLThread::hasWork() const {
    if (controlPending_.load(std::memory_order_relaxed)) return true;
    for (const WebGLChannel* ch : channels_)
        if (ch->hasWork()) return true;
    return false;
}

void WebGLThread::run() {
    nameThisThread();
    for (;;) {
        if (controlPending_.exchange(false, std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(controlMutex_);
            channels_.insert(channels_.end(), adds_.begin(), adds_.end());
            adds_.clear();
            if (stop_) {
                assert(channels_.empty() && "WebGLThread stopped with a context still alive");
                return;
            }
        }

        bool did = false;
        for (size_t i = 0; i < channels_.size();) {
            WebGLChannel* ch = channels_[i];
            if (ch->service()) did = true;
            if (!ch->closing()) {
                ++i;
                continue;
            }
            // Destroy replayed: drop the channel, then acknowledge (after
            // which its memory may go at any moment).
            channels_.erase(channels_.begin() + static_cast<std::ptrdiff_t>(i));
            {
                std::lock_guard<std::mutex> lock(controlMutex_);
                ch->destroyed_ = true;
            }
            destroyedCv_.notify_all();
        }
        if (did) continue;

        // Idle: more work usually follows within microseconds while a frame's
        // script runs, so spin a little before sleeping.
        const SpinDeadline until(kIdleSpin);
        bool work = false;
        for (uint32_t n = 1;; ++n) {
            if ((work = hasWork())) break;
            cpuRelax();
            if ((n & 15) == 0 && until.passed()) break;
        }
        if (!work) work_.sleepUnless([this] { return hasWork(); });
    }
}

}  // namespace bro::webgl
