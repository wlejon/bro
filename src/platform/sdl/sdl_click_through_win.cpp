// Click-through on Windows (sdl_click_through.h).
//
// A top-level window passes pointer input to the windows beneath it, other
// processes' included, only as WS_EX_LAYERED | WS_EX_TRANSPARENT, and then
// passes all of it: Windows has no input region that leaves the window's
// pixels alone (a window region clips what it shows as well, and
// WM_NCHITTEST's HTTRANSPARENT reaches only windows of the same thread). So
// the window's transparency to input follows the pointer: WS_EX_TRANSPARENT
// while the pointer is over a point where the page takes no input, cleared
// while it is over one where it does.
//
// While the window is transparent to input it gets no mouse messages, so the
// pointer is watched with a low-level mouse hook on a thread of its own (one
// for every click-through window of the process). The hook only notes where
// the pointer is over the window and wakes the main loop; the main thread
// asks the page about that point (Window::setInputTest: its own hit test)
// and flips the style in its next pump (SdlWindow::applyClickThrough). The
// hook never waits on the main thread: the whole desktop's pointer waits on
// the hook, and the main thread may be busy running a page. A style change
// sends the window messages, so it is made on the main thread too.
// WS_EX_LAYERED stays set while click-through is on; only WS_EX_TRANSPARENT
// toggles. With the pointer still nothing runs, but the page may change
// under it: the engine asks again after a frame that laid out or painted
// (refresh).
//
// A press the window took keeps it taking input until the release, so a drag
// that starts on a widget follows the pointer out of it.
#include "platform/sdl/sdl_click_through.h"
#include "util/log.h"
#include "util/main_loop_wake.h"

#include <SDL3/SDL.h>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <future>
#include <mutex>
#include <thread>

namespace bro::platform {

namespace {

class Win32ClickThrough;

// Every enabled tracker, and the hook thread that serves them.
std::mutex g_mu;
std::vector<Win32ClickThrough*> g_trackers;
std::thread g_thread;
DWORD g_threadId = 0;

LRESULT CALLBACK mouseProc(int code, WPARAM wp, LPARAM lp);

int64_t packPoint(POINT p) {
    return static_cast<int64_t>(static_cast<uint32_t>(p.x)) << 32 | static_cast<uint32_t>(p.y);
}
POINT unpackPoint(int64_t v) {
    return POINT{static_cast<LONG>(static_cast<int32_t>(v >> 32)), static_cast<LONG>(static_cast<int32_t>(v))};
}

class Win32ClickThrough final : public SdlClickThrough {
public:
    Win32ClickThrough(SDL_Window* window, HWND hwnd) : window_(window), hwnd_(hwnd) {}
    ~Win32ClickThrough() override { setEnabled(false); }

    bool wantsRegion() const override { return false; }
    void setTest(std::function<bool(float, float)> takesInput) override { test_ = std::move(takesInput); }

    void setEnabled(bool on) override {
        if (on == enabled_) return;
        enabled_ = on;
        if (on) {
            addLayered();
            {
                std::lock_guard<std::mutex> lock(g_mu);
                g_trackers.push_back(this);
            }
            startHook();
            refresh();
        } else {
            bool last = false;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                g_trackers.erase(std::remove(g_trackers.begin(), g_trackers.end(), this), g_trackers.end());
                last = g_trackers.empty();
            }
            if (last) stopHook();
            pending_.store(false);
            pressedInside_.store(0);
            passthroughDecided_ = false;
            setPassthrough(false);
            removeLayered();
        }
    }

    void refresh() override {
        if (!enabled_) return;
        POINT p{};
        if (!GetCursorPos(&p) || !ScreenToClient(hwnd_, &p)) return;
        RECT client{};
        GetClientRect(hwnd_, &client);
        if (PtInRect(&client, p)) {
            decide(p);
        } else if (!passthroughDecided_) {
            // Off the window it makes no difference to the pointer, but
            // whatever asks the desktop what is at a point (a drag's drop
            // target) is told the truth for the window's empty majority.
            setPassthrough(true);
        }
        passthroughDecided_ = true;
    }

    void apply() override {
        if (!enabled_ || !pending_.exchange(false)) return;
        decide(unpackPoint(point_.load()));
    }

    void restyle() override {
        if (!enabled_) return;
        addLayered();
        const bool pass = passthrough_;
        passthrough_ = !pass;
        setPassthrough(pass);
    }

    // Hook thread, g_mu held. Notes the pointer, never blocks.
    void onPointer(POINT screen, WPARAM msg) {
        POINT p = screen;
        if (!ScreenToClient(hwnd_, &p)) return;
        RECT client{};
        GetClientRect(hwnd_, &client);
        const bool over = PtInRect(&client, p) != 0;
        switch (msg) {
            case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
                // The window takes this press (it is not transparent to input here).
                if (over && !passthroughSeen_.load()) pressedInside_.fetch_add(1);
                return;
            case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
                if (pressedInside_.load() > 0 && pressedInside_.fetch_sub(1) == 1) break;  // decide again
                return;
            default:
                break;
        }
        if (!over) return;
        const int64_t packed = packPoint(p);
        if (point_.exchange(packed) == packed && msg == WM_MOUSEMOVE) return;
        if (!pending_.exchange(true)) util::wakeMainLoop();
    }

private:
    // Main thread: ask the page about client point `p` and follow its answer.
    void decide(POINT p) {
        bool takes = true;
        if (pressedInside_.load() == 0 && test_) {
            int ww = 0, wh = 0, pw = 0, ph = 0;
            SDL_GetWindowSize(window_, &ww, &wh);
            SDL_GetWindowSizeInPixels(window_, &pw, &ph);
            const float sx = pw > 0 ? static_cast<float>(ww) / pw : 1.0f;
            const float sy = ph > 0 ? static_cast<float>(wh) / ph : 1.0f;
            takes = test_((p.x + 0.5f) * sx, (p.y + 0.5f) * sy);
        }
        setPassthrough(!takes);
    }

    void addLayered() {
        const LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        if (ex & WS_EX_LAYERED) return;
        addedLayered_ = true;
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        // A layered window shows nothing until it has attributes: whole and
        // opaque, as it was (SDL's opacity, when set, is applied over this).
        SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    }

    void removeLayered() {
        if (!addedLayered_) return;
        addedLayered_ = false;
        BYTE alpha = 255;
        DWORD flags = 0;
        GetLayeredWindowAttributes(hwnd_, nullptr, &alpha, &flags);
        if ((flags & LWA_ALPHA) && alpha != 255) return;  // SDL's opacity needs it now
        const LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
    }

    void setPassthrough(bool pass) {
        if (pass == passthrough_) return;
        passthrough_ = pass;
        passthroughSeen_.store(pass);
        const LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        const LONG_PTR want = pass ? (ex | WS_EX_TRANSPARENT | WS_EX_LAYERED)
                                   : (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
        if (want != ex) SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, want);
    }

    static void startHook() {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_thread.joinable()) return;
        std::promise<DWORD> ready;
        auto started = ready.get_future();
        g_thread = std::thread([&ready] {
            MSG msg;
            PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // the queue, for WM_QUIT
            HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, GetModuleHandleW(nullptr), 0);
            if (!hook) LOG_WARN("click-through: SetWindowsHookEx failed (%lu)", GetLastError());
            ready.set_value(hook ? GetCurrentThreadId() : 0);
            if (!hook) return;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            }
            UnhookWindowsHookEx(hook);
        });
        g_threadId = started.get();
        if (!g_threadId) g_thread.join();
    }

    static void stopHook() {
        std::thread t;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (!g_thread.joinable()) return;
            PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
            t = std::move(g_thread);
            g_threadId = 0;
        }
        t.join();
    }

    SDL_Window* window_;
    HWND hwnd_;
    std::function<bool(float, float)> test_;
    bool enabled_ = false;
    bool addedLayered_ = false;
    bool passthrough_ = false;               // WS_EX_TRANSPARENT as set (main thread)
    bool passthroughDecided_ = false;        // decided once since enabled
    // Shared with the hook thread:
    std::atomic<bool> passthroughSeen_{false};
    std::atomic<int64_t> point_{INT64_MIN};  // the pointer over the window, client px
    std::atomic<bool> pending_{false};       // point_ not yet decided on
    std::atomic<int> pressedInside_{0};      // buttons held that the window took
};

LRESULT CALLBACK mouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        std::lock_guard<std::mutex> lock(g_mu);
        for (Win32ClickThrough* t : g_trackers) t->onPointer(m->pt, wp);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

} // namespace

std::unique_ptr<SdlClickThrough> SdlClickThrough::create(SDL_Window* window) {
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                         SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) return nullptr;
    return std::make_unique<Win32ClickThrough>(window, hwnd);
}

} // namespace bro::platform
