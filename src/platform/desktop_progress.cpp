#include "platform/desktop_progress.h"
#include "platform/desktop_platform.h"

#include <algorithm>
#include <atomic>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>

namespace {
ITaskbarList3* g_taskbar = nullptr;
bool g_taskbarTried = false;

ITaskbarList3* getTaskbar() {
    if (g_taskbarTried) return g_taskbar;
    g_taskbarTried = true;
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ITaskbarList3* t = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t)))) {
        if (SUCCEEDED(t->HrInit())) {
            g_taskbar = t;
        } else {
            t->Release();
        }
    }
    return g_taskbar;
}
} // namespace
#endif

namespace bro::platform::desktop {

namespace {
std::atomic<ProgressState> s_lastState{ProgressState::None};
std::atomic<int> s_lastValue{0};
}

bool setTaskbarProgress(SDL_Window* window, ProgressState state, int value) {
    const int clampedValue = std::max(0, std::min(100, value));
    s_lastState.store(state, std::memory_order_relaxed);
    s_lastValue.store(clampedValue, std::memory_order_relaxed);

    if (isHeadless()) {
        return true;
    }

#ifdef _WIN32
    HWND hwnd = hwndOf(window);
    ITaskbarList3* tb = hwnd ? getTaskbar() : nullptr;
    if (!tb) return false;

    TBPFLAG flag = TBPF_NOPROGRESS;
    switch (state) {
        case ProgressState::Normal: flag = TBPF_NORMAL; break;
        case ProgressState::Error: flag = TBPF_ERROR; break;
        case ProgressState::Indeterminate: flag = TBPF_INDETERMINATE; break;
        case ProgressState::Paused: flag = TBPF_PAUSED; break;
        case ProgressState::None:
        default: flag = TBPF_NOPROGRESS; break;
    }

    tb->SetProgressState(hwnd, flag);
    if (flag == TBPF_NORMAL || flag == TBPF_ERROR || flag == TBPF_PAUSED) {
        tb->SetProgressValue(hwnd, static_cast<ULONGLONG>(clampedValue), 100);
    }
    return true;
#else
    (void)window;
    // On Linux, Unity/GNOME launcher progress can be sent via D-Bus if available.
    // If unavailable or unsupported, safely return true since headless/Linux desktop
    // recorded the state.
    return true;
#endif
}

ProgressState getHeadlessProgressState() {
    return s_lastState.load(std::memory_order_relaxed);
}

int getHeadlessProgressValue() {
    return s_lastValue.load(std::memory_order_relaxed);
}

} // namespace bro::platform::desktop
