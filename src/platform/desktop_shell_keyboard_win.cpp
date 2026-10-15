// The Windows desktop shell's keyboard (desktop_hotkeys.h): a low-level
// keyboard hook on a thread of its own sees every key before the focused
// application and routes it through the hotkey state machine. The hook
// procedure only decides (route, then shellHookAction) and queues; callbacks
// and the keys a grab forwards run on the main thread (pumpHotkeyEvents),
// and injected keys go out from the hook thread's loop after the procedure
// has returned. Windows unhooks a procedure that keeps it waiting
// (LowLevelHooksTimeout), so nothing here may block for long.
#include "platform/desktop_hotkeys.h"
#include "platform/desktop_hotkeys_internal.h"
#include "platform/desktop_platform.h"
#include "platform/desktop_session.h"
#include "util/log.h"
#include "util/main_loop_wake.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <future>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace bro::platform::desktop {

namespace {

// Marks the keys this hook injects, so it lets them by (other injected
// input, an agent's or a remote viewer's, is routed like a device's).
constexpr ULONG_PTR kInjectedTag = 0x62726F68;  // 'broh'
// A virtual key with no meaning (unassigned): the "mask" between a
// swallowed key and the Win/Alt release.
constexpr WORD kMaskVk = 0xE8;
// Posted to the hook thread: inject the mask, then the release in lParam
// (vk | scan << 16 | extended << 31; 0: none).
constexpr UINT kMsgInject = WM_APP + 1;

std::atomic<bool> s_on{false};
std::mutex s_threadMu;  // starting / stopping the hook thread
std::thread s_thread;
DWORD s_threadId = 0;

// Hook thread only.
std::set<DWORD> s_heldMods;  // modifier virtual keys down
std::set<uint32_t> s_down;   // codes down (auto-repeat detection)
bool s_masked = false;       // a key was swallowed since Win/Alt went down

uint32_t modifierBitOfVk(DWORD vk) {
    switch (vk) {
        case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return hotkey_mod::Ctrl;
        case VK_LMENU: case VK_RMENU: case VK_MENU: return hotkey_mod::Alt;
        case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: return hotkey_mod::Shift;
        case VK_LWIN: case VK_RWIN: return hotkey_mod::Meta;
        default: return 0;
    }
}

uint32_t heldMods() {
    uint32_t m = 0;
    for (DWORD vk : s_heldMods) m |= modifierBitOfVk(vk);
    return m;
}

// The accelerator key name of a virtual key (desktop_hotkeys.cpp's
// spellings; punctuation by its US-layout position), "" if none.
std::string keyNameOfVk(DWORD vk) {
    if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk - 'A' + 'a'));
    if (vk >= '0' && vk <= '9') return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "f" + std::to_string(vk - VK_F1 + 1);
    switch (vk) {
        case VK_SPACE: return "space";
        case VK_TAB: return "tab";
        case VK_RETURN: return "enter";
        case VK_ESCAPE: return "escape";
        case VK_BACK: return "backspace";
        case VK_DELETE: return "delete";
        case VK_INSERT: return "insert";
        case VK_HOME: return "home";
        case VK_END: return "end";
        case VK_PRIOR: return "pageup";
        case VK_NEXT: return "pagedown";
        case VK_UP: return "up";
        case VK_DOWN: return "down";
        case VK_LEFT: return "left";
        case VK_RIGHT: return "right";
        case VK_VOLUME_UP: return "volumeup";
        case VK_VOLUME_DOWN: return "volumedown";
        case VK_VOLUME_MUTE: return "volumemute";
        case VK_MEDIA_PLAY_PAUSE: return "mediaplaypause";
        case VK_MEDIA_NEXT_TRACK: return "medianexttrack";
        case VK_MEDIA_PREV_TRACK: return "mediaprevioustrack";
        case VK_MEDIA_STOP: return "mediastop";
        case VK_SNAPSHOT: return "printscreen";
        case VK_OEM_COMMA: return ",";
        case VK_OEM_PERIOD: return ".";
        case VK_OEM_MINUS: return "-";
        case VK_OEM_PLUS: return "=";
        case VK_OEM_1: return ";";
        case VK_OEM_2: return "/";
        case VK_OEM_3: return "`";
        case VK_OEM_4: return "[";
        case VK_OEM_5: return "\\";
        case VK_OEM_6: return "]";
        case VK_OEM_7: return "'";
        default: return {};
    }
}

bool shellHasForeground() {
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void injectMaskThen(LPARAM release) {
    INPUT in[3] = {};
    UINT n = 0;
    for (bool up : {false, true}) {
        INPUT& i = in[n++];
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = kMaskVk;
        i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        i.ki.dwExtraInfo = kInjectedTag;
    }
    if (release) {
        INPUT& i = in[n++];
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = static_cast<WORD>(release & 0xFFFF);
        i.ki.wScan = static_cast<WORD>((release >> 16) & 0x7FFF);
        i.ki.dwFlags = KEYEVENTF_KEYUP | ((release >> 31) & 1 ? KEYEVENTF_EXTENDEDKEY : 0);
        i.ki.dwExtraInfo = kInjectedTag;
    }
    SendInput(n, in, sizeof(INPUT));
}

LRESULT CALLBACK keyboardProc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
    const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
    if (k->dwExtraInfo == kInjectedTag || k->vkCode == kMaskVk) return CallNextHookEx(nullptr, code, wp, lp);

    const bool down = !(k->flags & LLKHF_UP);
    const bool extended = (k->flags & LLKHF_EXTENDED) != 0;
    const uint16_t scan = static_cast<uint16_t>((extended ? 0xE000 : 0) | (k->scanCode & 0xFF));
    HotkeyKey key;
    key.code = k->scanCode ? scan : 0x10000u | k->vkCode;
    key.modifierKey = modifierBitOfVk(k->vkCode);
    if (!key.modifierKey) key.key = keyNameOfVk(k->vkCode);
    key.down = down;
    if (down) {
        key.repeat = !s_down.insert(key.code).second;
        if (key.modifierKey) s_heldMods.insert(k->vkCode);
    } else {
        s_down.erase(key.code);
        if (key.modifierKey) s_heldMods.erase(k->vkCode);
    }
    key.mods = heldMods();

    const HotkeyKeyResult routed = routeHotkeyKeyDeferred(key);
    const ShellHookAction act = shellHookAction(key, routed, shellHasForeground(), s_masked);
    if (act.mask) s_masked = true;
    if (act.replayRelease) {
        const LPARAM release = static_cast<LPARAM>(k->vkCode & 0xFFFF) |
                               static_cast<LPARAM>(k->scanCode & 0x7FFF) << 16 |
                               static_cast<LPARAM>(extended ? 1 : 0) << 31;
        PostThreadMessageW(GetCurrentThreadId(), kMsgInject, 0, release);
    }
    if (!(key.mods & (hotkey_mod::Alt | hotkey_mod::Meta))) s_masked = false;
    if (act.forward) {
        if (ShellKeySink sink = shellKeySink()) {
            const uint32_t evdev = scanToEvdev(scan);
            if (evdev)
                queueHotkeyTask([sink = std::move(sink), evdev, down, repeat = key.repeat, mods = key.mods] {
                    sink(evdev, down, repeat, mods);
                });
        }
    }
    if (routed.fired || act.forward) util::wakeMainLoop();
    return act.suppress ? 1 : CallNextHookEx(nullptr, code, wp, lp);
}

void hookThread(std::promise<bool>* ready) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // the thread's queue, for WM_QUIT
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardProc, GetModuleHandleW(nullptr), 0);
    if (!hook) LOG_WARN("desktop shell keyboard: SetWindowsHookEx failed (%lu)", GetLastError());
    s_heldMods.clear();
    s_down.clear();
    s_masked = false;
    ready->set_value(hook != nullptr);
    if (!hook) return;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kMsgInject) injectMaskThen(msg.lParam);
    }
    UnhookWindowsHookEx(hook);
}

}  // namespace

void setDesktopShellKeyboard(bool on) {
    if (s_on.exchange(on) == on) return;
    LOG_INFO("desktop shell keyboard: %s", on ? "on" : "off");
    updateShellKeyboardHook();
}

bool desktopShellKeyboard() { return s_on.load(); }

void updateShellKeyboardHook() {
    const bool want = s_on.load() && !isHeadless() && registeredHotkeyCount() > 0;
    std::lock_guard<std::mutex> lock(s_threadMu);
    if (want == s_thread.joinable()) return;
    if (want) {
        std::promise<bool> ready;
        auto started = ready.get_future();
        s_thread = std::thread(hookThread, &ready);
        s_threadId = GetThreadId(s_thread.native_handle());
        if (!started.get()) {
            s_thread.join();
            s_threadId = 0;
        }
        return;
    }
    PostThreadMessageW(s_threadId, WM_QUIT, 0, 0);
    s_thread.join();
    s_threadId = 0;
    resetHotkeyKeyState();
}

}  // namespace bro::platform::desktop
