#include "platform/desktop_hotkeys.h"
#include "platform/desktop_platform.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace bro::platform::desktop {

namespace {

struct HotkeyEntry {
    uint32_t id = 0;
    std::string normalized;
    std::function<void()> callback;
#ifdef _WIN32
    HWND hwnd = nullptr;
#endif
};

std::mutex s_hotkeyMutex;
std::unordered_map<uint32_t, HotkeyEntry> s_hotkeys;
std::atomic<uint32_t> s_nextHotkeyId{1};
std::deque<std::function<void()>> s_pendingCallbacks;

std::string normalizeAccelerator(const std::string& input) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : input) {
        if (c == '+') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            current += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (!current.empty()) parts.push_back(current);

    bool ctrl = false, alt = false, shift = false, meta = false;
    std::string key;

    for (const auto& p : parts) {
        if (p == "ctrl" || p == "control") {
            ctrl = true;
        } else if (p == "commandorcontrol" || p == "cmdorctrl") {
#ifdef __APPLE__
            meta = true;
#else
            ctrl = true;
#endif
        } else if (p == "alt" || p == "option") {
            alt = true;
        } else if (p == "shift") {
            shift = true;
        } else if (p == "cmd" || p == "command" || p == "meta" || p == "super" || p == "win") {
            meta = true;
        } else {
            key = p;
        }
    }

    std::string result;
    if (ctrl) result += "ctrl+";
    if (alt) result += "alt+";
    if (shift) result += "shift+";
    if (meta) result += "meta+";
    result += key;
    return result;
}

#ifdef _WIN32
bool parseWinModifiersAndKey(const std::string& normalized, UINT& fsModifiers, UINT& vk) {
    fsModifiers = 0;
    vk = 0;

    std::vector<std::string> parts;
    std::string current;
    for (char c : normalized) {
        if (c == '+') {
            parts.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) parts.push_back(current);

    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& p = parts[i];
        if (p == "ctrl") fsModifiers |= MOD_CONTROL;
        else if (p == "alt") fsModifiers |= MOD_ALT;
        else if (p == "shift") fsModifiers |= MOD_SHIFT;
        else if (p == "meta") fsModifiers |= MOD_WIN;
        else {
            // Key
            if (p.size() == 1 && std::isalnum(static_cast<unsigned char>(p[0]))) {
                vk = static_cast<UINT>(std::toupper(static_cast<unsigned char>(p[0])));
            } else if (p.size() >= 2 && p[0] == 'f') {
                int fnNum = std::atoi(p.c_str() + 1);
                if (fnNum >= 1 && fnNum <= 24) vk = VK_F1 + (fnNum - 1);
            } else if (p == "space") vk = VK_SPACE;
            else if (p == "tab") vk = VK_TAB;
            else if (p == "enter" || p == "return") vk = VK_RETURN;
            else if (p == "escape" || p == "esc") vk = VK_ESCAPE;
            else if (p == "backspace") vk = VK_BACK;
            else if (p == "delete") vk = VK_DELETE;
            else if (p == "insert") vk = VK_INSERT;
            else if (p == "home") vk = VK_HOME;
            else if (p == "end") vk = VK_END;
            else if (p == "pageup") vk = VK_PRIOR;
            else if (p == "pagedown") vk = VK_NEXT;
            else if (p == "up") vk = VK_UP;
            else if (p == "down") vk = VK_DOWN;
            else if (p == "left") vk = VK_LEFT;
            else if (p == "right") vk = VK_RIGHT;
        }
    }
    return vk != 0;
}
#endif

} // namespace

uint32_t registerGlobalHotkey(
    SDL_Window* window,
    const std::string& accelerator,
    std::function<void()> callback
) {
    if (accelerator.empty() || !callback) return 0;

    std::string norm = normalizeAccelerator(accelerator);
    uint32_t id = s_nextHotkeyId.fetch_add(1, std::memory_order_relaxed);

    HotkeyEntry entry;
    entry.id = id;
    entry.normalized = norm;
    entry.callback = std::move(callback);

#ifdef _WIN32
    if (!isHeadless() && window) {
        HWND hwnd = hwndOf(window);
        entry.hwnd = hwnd;
        UINT fsModifiers = 0, vk = 0;
        if (parseWinModifiersAndKey(norm, fsModifiers, vk)) {
            RegisterHotKey(hwnd, static_cast<int>(id), fsModifiers, vk);
        }
    }
#else
    (void)window;
#endif

    {
        std::lock_guard<std::mutex> lock(s_hotkeyMutex);
        s_hotkeys[id] = std::move(entry);
    }
    return id;
}

bool unregisterGlobalHotkey(uint32_t id) {
    std::lock_guard<std::mutex> lock(s_hotkeyMutex);
    auto it = s_hotkeys.find(id);
    if (it == s_hotkeys.end()) return false;

#ifdef _WIN32
    if (it->second.hwnd) {
        UnregisterHotKey(it->second.hwnd, static_cast<int>(id));
    }
#endif

    s_hotkeys.erase(it);
    return true;
}

void unregisterAllGlobalHotkeys() {
    std::lock_guard<std::mutex> lock(s_hotkeyMutex);
#ifdef _WIN32
    for (const auto& [id, entry] : s_hotkeys) {
        if (entry.hwnd) {
            UnregisterHotKey(entry.hwnd, static_cast<int>(id));
        }
    }
#endif
    s_hotkeys.clear();
}

bool simulateGlobalHotkey(const std::string& accelerator) {
    std::string norm = normalizeAccelerator(accelerator);
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(s_hotkeyMutex);
        for (const auto& [id, entry] : s_hotkeys) {
            if (entry.normalized == norm) {
                cb = entry.callback;
                break;
            }
        }
    }
    if (cb) {
        cb();
        return true;
    }
    return false;
}

bool simulateGlobalHotkeyId(uint32_t id) {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(s_hotkeyMutex);
        auto it = s_hotkeys.find(id);
        if (it != s_hotkeys.end()) {
            cb = it->second.callback;
        }
    }
    if (cb) {
        cb();
        return true;
    }
    return false;
}

void pumpHotkeyEvents() {
    std::vector<std::function<void()>> toRun;
    {
        std::lock_guard<std::mutex> lock(s_hotkeyMutex);
        while (!s_pendingCallbacks.empty()) {
            toRun.push_back(std::move(s_pendingCallbacks.front()));
            s_pendingCallbacks.pop_front();
        }
    }
    for (auto& fn : toRun) {
        if (fn) fn();
    }
}

} // namespace bro::platform::desktop
