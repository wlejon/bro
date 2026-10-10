// The SDL window system: windows, events, displays and the keyboard layout
// through SDL3's video subsystem; power, theme, URLs and gamepads through
// the rest of SDL.
#include "platform/backends.h"
#include "platform/clipboard.h"
#include "platform/displays.h"
#include "platform/event_loop.h"
#include "platform/gamepads.h"
#include "platform/keyboard.h"
#include "platform/sdl/sdl_backend.h"
#include "platform/sdl/sdl_window.h"
#include "platform/system_info.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <unordered_map>

namespace bro::platform {

namespace {

class SdlDisplays final : public Displays {
public:
    std::vector<DisplayInfo> list(const Window* window) override {
        std::vector<DisplayInfo> result;

        int count = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        if (!displays) return result;

        SDL_DisplayID primary = SDL_GetPrimaryDisplay();
        SDL_DisplayID current = window ? static_cast<SDL_DisplayID>(window->currentDisplay()) : 0;

        for (int i = 0; i < count; i++) {
            SDL_DisplayID id = displays[i];
            DisplayInfo info;
            info.id = id;
            if (const char* name = SDL_GetDisplayName(id)) info.name = name;

            SDL_Rect bounds{};
            if (SDL_GetDisplayBounds(id, &bounds)) {
                info.x = bounds.x; info.y = bounds.y;
                info.width = bounds.w; info.height = bounds.h;
            }
            // Usable bounds exclude the taskbar/dock; fall back to full bounds
            // if the query fails (some minimal Wayland compositors).
            SDL_Rect usable{};
            if (SDL_GetDisplayUsableBounds(id, &usable)) {
                info.workX = usable.x; info.workY = usable.y;
                info.workWidth = usable.w; info.workHeight = usable.h;
            } else {
                info.workX = info.x; info.workY = info.y;
                info.workWidth = info.width; info.workHeight = info.height;
            }

            if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(id)) {
                info.refreshRate = mode->refresh_rate;
            }
            float scale = SDL_GetDisplayContentScale(id);
            info.contentScale = scale > 0.0f ? scale : 1.0f;

            info.isPrimary = (id == primary);
            info.isCurrent = (id == current);
            result.push_back(std::move(info));
        }
        SDL_free(displays);
        return result;
    }

    std::vector<DisplayModeInfo> fullscreenModes(uint32_t displayId) override {
        std::vector<DisplayModeInfo> result;
        int count = 0;
        SDL_DisplayMode** modes =
            SDL_GetFullscreenDisplayModes(static_cast<SDL_DisplayID>(displayId), &count);
        if (!modes) return result;
        for (int i = 0; i < count; i++) {
            if (modes[i]) result.push_back({modes[i]->w, modes[i]->h, modes[i]->refresh_rate});
        }
        SDL_free(modes);
        return result;
    }
};

// SDL's keymap is the active layout once video is up; before that (and
// without video at all) SDL answers from its US-QWERTY default, which is
// defaultKeyFromScancode's table.
class SdlKeyboard final : public Keyboard {
public:
    KeyMods modState() override { return static_cast<KeyMods>(SDL_GetModState()); }

    Keycode eventKeycode(Scancode scancode) override {
        return static_cast<Keycode>(
            SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(scancode), SDL_KMOD_NONE, true));
    }

    Keycode layoutKeycode(Scancode scancode, KeyMods mods) override {
        return static_cast<Keycode>(SDL_GetKeyFromScancode(
            static_cast<SDL_Scancode>(scancode), static_cast<SDL_Keymod>(mods), false));
    }

    Scancode scancodeFromKey(Keycode key, KeyMods* mods) override {
        SDL_Keymod m = SDL_KMOD_NONE;
        const SDL_Scancode s = SDL_GetScancodeFromKey(static_cast<SDL_Keycode>(key), mods ? &m : nullptr);
        if (mods) *mods = static_cast<KeyMods>(m);
        return static_cast<Scancode>(s);
    }
};

class SdlSystemInfo final : public SystemInfo {
public:
    PowerInfo power() override {
        PowerInfo info;
        int secs = -1, pct = -1;
        switch (SDL_GetPowerInfo(&secs, &pct)) {
            case SDL_POWERSTATE_ON_BATTERY: info.state = PowerInfo::State::OnBattery; break;
            case SDL_POWERSTATE_CHARGING:   info.state = PowerInfo::State::Charging; break;
            case SDL_POWERSTATE_CHARGED:    info.state = PowerInfo::State::Charged; break;
            case SDL_POWERSTATE_NO_BATTERY: info.state = PowerInfo::State::NoBattery; break;
            case SDL_POWERSTATE_UNKNOWN:
            case SDL_POWERSTATE_ERROR:
            default:                        info.state = PowerInfo::State::Unknown; break;
        }
        info.secondsLeft = secs;
        info.percent = pct;
        return info;
    }

    SystemTheme theme() override {
        switch (SDL_GetSystemTheme()) {
            case SDL_SYSTEM_THEME_DARK:  return SystemTheme::Dark;
            case SDL_SYSTEM_THEME_LIGHT: return SystemTheme::Light;
            default:                     return SystemTheme::Unknown;
        }
    }

    bool openUrl(const std::string& url, std::string* error) override {
        if (SDL_OpenURL(url.c_str())) return true;
        if (error) *error = SDL_GetError();
        return false;
    }
};

class SdlGamepads final : public Gamepads {
public:
    void start() override { sdlStartGamepadsOnly(); }

    bool open(uint32_t instanceId, std::string* error) override {
        if (m_open.count(instanceId)) return true;
        SDL_Gamepad* pad = SDL_OpenGamepad(static_cast<SDL_JoystickID>(instanceId));
        if (!pad) {
            if (error) *error = SDL_GetError();
            return false;
        }
        m_open[instanceId] = pad;
        return true;
    }

    void close(uint32_t instanceId) override {
        auto it = m_open.find(instanceId);
        if (it == m_open.end()) return;
        SDL_CloseGamepad(it->second);
        m_open.erase(it);
    }

    std::string name(uint32_t instanceId) override {
        SDL_Gamepad* pad = find(instanceId);
        const char* n = pad ? SDL_GetGamepadName(pad) : nullptr;
        return n ? n : "";
    }

    bool rumble(uint32_t instanceId, float strong, float weak, int durationMs) override {
        SDL_Gamepad* pad = find(instanceId);
        return pad && SDL_RumbleGamepad(pad, static_cast<Uint16>(strong * 0xFFFF),
                                        static_cast<Uint16>(weak * 0xFFFF),
                                        static_cast<Uint32>(durationMs));
    }

    bool rumbleTriggers(uint32_t instanceId, float left, float right, int durationMs) override {
        SDL_Gamepad* pad = find(instanceId);
        return pad && SDL_RumbleGamepadTriggers(pad, static_cast<Uint16>(left * 0xFFFF),
                                                static_cast<Uint16>(right * 0xFFFF),
                                                static_cast<Uint32>(durationMs));
    }

private:
    SDL_Gamepad* find(uint32_t instanceId) {
        auto it = m_open.find(instanceId);
        return it == m_open.end() ? nullptr : it->second;
    }
    std::unordered_map<uint32_t, SDL_Gamepad*> m_open;
};

class SdlWindowSystem final : public WindowSystem {
public:
    const char* name() const override { return "sdl"; }

    std::string driverName() const override {
        const char* d = SDL_GetCurrentVideoDriver();
        return d ? d : "";
    }

    std::unique_ptr<Window> createWindow(const WindowConfig& config) override {
        return std::make_unique<SdlWindow>(config);
    }

    std::unique_ptr<EventLoop> createEventLoop() override { return createSdlEventLoop(); }

    void pumpEvents() override { SDL_PumpEvents(); }

    std::vector<std::string> vulkanInstanceExtensions() override {
        std::vector<std::string> out;
        uint32_t n = 0;
        char const* const* exts = SDL_Vulkan_GetInstanceExtensions(&n);
        if (!exts) {
            LOG_ERROR("Vulkan: SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
            return out;
        }
        for (uint32_t i = 0; i < n; ++i) out.emplace_back(exts[i]);
        return out;
    }

    Displays& displays() override { return m_displays; }
    Keyboard& keyboard() override { return m_keyboard; }
    Clipboard& clipboard() override { return sdlClipboard(); }
    DialogBackend& dialogs() override { return sdlDialogs(); }
    SystemInfo& systemInfo() override { return sdlSystemInfo(); }
    Gamepads& gamepads() override { return sdlGamepads(); }

private:
    SdlDisplays m_displays;
    SdlKeyboard m_keyboard;
};

}  // namespace

SystemInfo& sdlSystemInfo() {
    static SdlSystemInfo s;
    return s;
}

Gamepads& sdlGamepads() {
    static SdlGamepads g;
    return g;
}

WindowSystem& sdlWindowSystem() {
    static SdlWindowSystem s;
    return s;
}

}  // namespace bro::platform
