#include "platform/desktop_hotkeys.h"
#include "platform/desktop_platform.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <mutex>
#include <set>
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
    uint32_t mods = 0;      // hotkey_mod bits
    std::string key;        // "" for a modifier tap
    bool grab = false;
    HotkeyCallback callback;
#ifdef _WIN32
    HWND hwnd = nullptr;
#endif
};

std::mutex s_hotkeyMutex;
std::unordered_map<uint32_t, HotkeyEntry> s_hotkeys;
std::atomic<uint32_t> s_nextHotkeyId{1};
std::deque<std::function<void()>> s_pendingCallbacks;

// Key-routing state (routeHotkeyKey), guarded by s_hotkeyMutex.
struct KeyState {
    uint32_t held = 0;           // modifier bits held
    uint32_t tapMods = 0;        // modifiers pressed since none was held
    bool tapClean = false;       // ... with no other key or button in between
    std::set<uint32_t> consumed; // codes whose press matched a chord
    std::set<uint32_t> keysDown; // non-modifier codes held
    bool grab = false;
    uint32_t grabMods = 0;
} s_keys;

struct ParsedAccel {
    uint32_t mods = 0;
    std::string key;
};

std::string canonicalKey(const std::string& k) {
    static const std::unordered_map<std::string, std::string> aliases = {
        {"return", "enter"}, {"esc", "escape"}, {"spacebar", "space"},
        {"arrowup", "up"}, {"arrowdown", "down"}, {"arrowleft", "left"}, {"arrowright", "right"},
        {"del", "delete"}, {"ins", "insert"}, {"pgup", "pageup"}, {"pgdn", "pagedown"},
        {"audiovolumeup", "volumeup"}, {"audiovolumedown", "volumedown"},
        {"audiovolumemute", "volumemute"}, {"mute", "volumemute"},
        {"comma", ","}, {"period", "."}, {"slash", "/"}, {"semicolon", ";"},
        {"minus", "-"}, {"equal", "="}, {"backquote", "`"},
    };
    auto it = aliases.find(k);
    return it == aliases.end() ? k : it->second;
}

ParsedAccel parseAccelerator(const std::string& input) {
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

    ParsedAccel a;
    for (const auto& p : parts) {
        if (p == "ctrl" || p == "control") {
            a.mods |= hotkey_mod::Ctrl;
        } else if (p == "commandorcontrol" || p == "cmdorctrl") {
#ifdef __APPLE__
            a.mods |= hotkey_mod::Meta;
#else
            a.mods |= hotkey_mod::Ctrl;
#endif
        } else if (p == "alt" || p == "option") {
            a.mods |= hotkey_mod::Alt;
        } else if (p == "shift") {
            a.mods |= hotkey_mod::Shift;
        } else if (p == "cmd" || p == "command" || p == "meta" || p == "super" || p == "win") {
            a.mods |= hotkey_mod::Meta;
        } else {
            a.key = canonicalKey(p);
        }
    }
    return a;
}

std::string canonical(const ParsedAccel& a) {
    std::string result;
    if (a.mods & hotkey_mod::Ctrl) result += "ctrl+";
    if (a.mods & hotkey_mod::Alt) result += "alt+";
    if (a.mods & hotkey_mod::Shift) result += "shift+";
    if (a.mods & hotkey_mod::Meta) result += "meta+";
    result += a.key;
    return result;
}

std::string normalizeAccelerator(const std::string& input) {
    return canonical(parseAccelerator(input));
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
    HotkeyCallback callback,
    HotkeyOptions options
) {
    if (accelerator.empty() || !callback) return 0;

    ParsedAccel parsed = parseAccelerator(accelerator);
    if (parsed.mods == 0 && parsed.key.empty()) return 0;
    std::string norm = canonical(parsed);
    uint32_t id = s_nextHotkeyId.fetch_add(1, std::memory_order_relaxed);

    HotkeyEntry entry;
    entry.id = id;
    entry.normalized = norm;
    entry.mods = parsed.mods;
    entry.key = parsed.key;
    entry.grab = options.grab;
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
    HotkeyCallback cb;
    uint32_t hid = 0;
    {
        std::lock_guard<std::mutex> lock(s_hotkeyMutex);
        for (const auto& [id, entry] : s_hotkeys) {
            if (entry.normalized == norm) {
                cb = entry.callback;
                hid = id;
                break;
            }
        }
    }
    if (cb) {
        cb(hid);
        return true;
    }
    return false;
}

bool simulateGlobalHotkeyId(uint32_t id) {
    HotkeyCallback cb;
    {
        std::lock_guard<std::mutex> lock(s_hotkeyMutex);
        auto it = s_hotkeys.find(id);
        if (it != s_hotkeys.end()) {
            cb = it->second.callback;
        }
    }
    if (cb) {
        cb(id);
        return true;
    }
    return false;
}

// ---- key routing -----------------------------------------------------------

namespace {

uint32_t modifierBitOfKeycode(int32_t kc) {
    switch (kc) {
        case SDLK_LCTRL: case SDLK_RCTRL: return hotkey_mod::Ctrl;
        case SDLK_LALT: case SDLK_RALT: return hotkey_mod::Alt;
        case SDLK_LSHIFT: case SDLK_RSHIFT: return hotkey_mod::Shift;
        case SDLK_LGUI: case SDLK_RGUI: return hotkey_mod::Meta;
        default: return 0;
    }
}

std::string keyNameOfKeycode(int32_t kc) {
    if ((kc >= 'a' && kc <= 'z') || (kc >= '0' && kc <= '9')) return std::string(1, static_cast<char>(kc));
    if (kc >= SDLK_F1 && kc <= SDLK_F12) return "f" + std::to_string(kc - SDLK_F1 + 1);
    if (kc >= SDLK_F13 && kc <= SDLK_F24) return "f" + std::to_string(kc - SDLK_F13 + 13);
    switch (kc) {
        case SDLK_SPACE: return "space";
        case SDLK_TAB: return "tab";
        case SDLK_RETURN: case SDLK_KP_ENTER: return "enter";
        case SDLK_ESCAPE: return "escape";
        case SDLK_BACKSPACE: return "backspace";
        case SDLK_DELETE: return "delete";
        case SDLK_INSERT: return "insert";
        case SDLK_HOME: return "home";
        case SDLK_END: return "end";
        case SDLK_PAGEUP: return "pageup";
        case SDLK_PAGEDOWN: return "pagedown";
        case SDLK_UP: return "up";
        case SDLK_DOWN: return "down";
        case SDLK_LEFT: return "left";
        case SDLK_RIGHT: return "right";
        case SDLK_VOLUMEUP: return "volumeup";
        case SDLK_VOLUMEDOWN: return "volumedown";
        case SDLK_MUTE: return "volumemute";
        case SDLK_MEDIA_PLAY_PAUSE: return "mediaplaypause";
        case SDLK_MEDIA_NEXT_TRACK: return "medianexttrack";
        case SDLK_MEDIA_PREVIOUS_TRACK: return "mediaprevioustrack";
        case SDLK_MEDIA_STOP: return "mediastop";
        case SDLK_PRINTSCREEN: return "printscreen";
        default: break;
    }
    if (kc > 0x20 && kc < 0x7f) return std::string(1, static_cast<char>(kc));
    return {};
}

// The registered entry `norm` names, or null. Caller holds s_hotkeyMutex.
const HotkeyEntry* findEntry(const std::string& norm) {
    for (const auto& [id, entry] : s_hotkeys)
        if (entry.normalized == norm) return &entry;
    return nullptr;
}

} // namespace

HotkeyKey hotkeyKeyFromSdl(int32_t keycode, int32_t scancode, int32_t sdlMods, bool down, bool repeat) {
    HotkeyKey k;
    k.code = static_cast<uint32_t>(scancode ? scancode : keycode);
    k.modifierKey = modifierBitOfKeycode(keycode);
    if (!k.modifierKey) k.key = keyNameOfKeycode(keycode);
    if (sdlMods & SDL_KMOD_CTRL) k.mods |= hotkey_mod::Ctrl;
    if (sdlMods & SDL_KMOD_ALT) k.mods |= hotkey_mod::Alt;
    if (sdlMods & SDL_KMOD_SHIFT) k.mods |= hotkey_mod::Shift;
    if (sdlMods & SDL_KMOD_GUI) k.mods |= hotkey_mod::Meta;
    k.down = down;
    k.repeat = repeat;
    return k;
}

HotkeyKey hotkeyKeyFromNames(const std::string& key, const std::string& mods, uint32_t code,
                             bool down, bool repeat) {
    HotkeyKey k;
    k.code = code;
    ParsedAccel asKey = parseAccelerator(key);
    if (asKey.key.empty()) k.modifierKey = asKey.mods;
    else k.key = asKey.key;
    k.mods = parseAccelerator(mods).mods;
    k.down = down;
    k.repeat = repeat;
    return k;
}

HotkeyKeyResult routeHotkeyKey(const HotkeyKey& k) {
    HotkeyKeyResult res;
    HotkeyCallback cb;
    uint32_t firedId = 0;
    {
        std::lock_guard<std::mutex> lock(s_hotkeyMutex);
        KeyState& st = s_keys;
        if (k.modifierKey) {
            if (k.down) {
                if (!k.repeat) {
                    if (st.held == 0) {
                        st.tapMods = 0;
                        st.tapClean = st.keysDown.empty();
                    }
                    st.tapMods |= k.modifierKey;
                    st.held |= k.modifierKey;
                }
                res.grabbed = st.grab;
            } else {
                st.held &= ~k.modifierKey;
                if (st.held == 0 && st.tapClean && st.tapMods) {
                    if (const HotkeyEntry* e = findEntry(canonical(ParsedAccel{st.tapMods, {}}))) {
                        cb = e->callback;
                        firedId = e->id;
                    }
                }
                if (st.held == 0) st.tapClean = false;
                // The release that ends a grab still belongs to it.
                res.grabbed = st.grab;
                if (st.grab && (st.held & st.grabMods) == 0) st.grab = false;
            }
        } else if (k.down) {
            st.tapClean = false;
            if (k.repeat) {
                res.consumed = st.consumed.count(k.code) != 0;
            } else {
                st.keysDown.insert(k.code);
                const HotkeyEntry* e = k.key.empty() ? nullptr : findEntry(canonical(ParsedAccel{k.mods, k.key}));
                if (e) {
                    st.consumed.insert(k.code);
                    res.consumed = true;
                    cb = e->callback;
                    firedId = e->id;
                    if (e->grab && e->mods) {
                        st.grab = true;
                        st.grabMods = e->mods;
                    }
                }
            }
            res.grabbed = st.grab;
        } else {
            st.keysDown.erase(k.code);
            res.consumed = st.consumed.erase(k.code) != 0;
            res.grabbed = st.grab;
        }
    }
    if (cb) {
        res.fired = true;
        cb(firedId);
    }
    return res;
}

void cancelHotkeyTap() {
    std::lock_guard<std::mutex> lock(s_hotkeyMutex);
    s_keys.tapClean = false;
}

bool hotkeyGrabActive() {
    std::lock_guard<std::mutex> lock(s_hotkeyMutex);
    return s_keys.grab;
}

void resetHotkeyKeyState() {
    std::lock_guard<std::mutex> lock(s_hotkeyMutex);
    s_keys = KeyState{};
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
