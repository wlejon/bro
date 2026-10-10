// The DRM window system: bro is the display server. It presents through KMS
// (engine_init_drm / the DRM presenter) and reads the seat's devices through
// libinput (DrmInputPlatform), so it has no desktop windows and no event
// loop of its own here. What a desktop session would supply is answered
// locally: the keyboard layout is US-QWERTY (the same table SDL falls back
// to without a keymap), the clipboard is this process's, and there are no
// displays or native dialogs to show. Power, theme, URL opening and
// gamepads use SDL as a library — none of it starts SDL's video subsystem.
#include "platform/backends.h"
#include "platform/clipboard.h"
#include "platform/displays.h"
#include "platform/event_loop.h"
#include "platform/keyboard.h"
#include "platform/sdl/sdl_backend.h"
#include "platform/window.h"

#include <mutex>
#include <stdexcept>

namespace bro::platform {

namespace {

class DrmDisplays final : public Displays {
public:
    std::vector<DisplayInfo> list(const Window*) override { return {}; }
    std::vector<DisplayModeInfo> fullscreenModes(uint32_t) override { return {}; }
};

// DRM key events carry their modifiers (DrmInputEvent); there is no
// separate modifier query.
class DrmKeyboard final : public Keyboard {
public:
    KeyMods modState() override { return kmod::None; }
    Keycode eventKeycode(Scancode scancode) override { return defaultKeyFromScancode(scancode); }
    Keycode layoutKeycode(Scancode scancode, KeyMods mods) override {
        return defaultKeyFromScancode(scancode, mods);
    }
    Scancode scancodeFromKey(Keycode key, KeyMods* mods) override {
        return defaultScancodeFromKey(key, mods);
    }
};

class NoDialogs final : public DialogBackend {
public:
    std::optional<bool> messageBox(Window*, const std::string&, bool) override { return std::nullopt; }
    bool fileDialog(Window*, const FileDialogRequest&, const std::function<void()>&,
                    std::vector<std::string>&, std::string& refusal) override {
        refusal = "file dialog refused: no native dialogs on the DRM display";
        return false;
    }
};

class DrmWindowSystem final : public WindowSystem {
public:
    const char* name() const override { return "drm"; }
    std::string driverName() const override { return "kms"; }

    std::unique_ptr<Window> createWindow(const WindowConfig&) override {
        throw std::runtime_error("the DRM window system has no desktop windows");
    }
    std::unique_ptr<EventLoop> createEventLoop() override { return nullptr; }
    void pumpEvents() override {}
    std::vector<std::string> vulkanInstanceExtensions() override { return {}; }

    Displays& displays() override { return m_displays; }
    Keyboard& keyboard() override { return m_keyboard; }
    // One clipboard for the shell, its page and its terminals: this
    // process's. Clients of the shell's Wayland compositor keep theirs in
    // the compositor.
    Clipboard& clipboard() override { return localClipboard(); }
    DialogBackend& dialogs() override { return m_dialogs; }
    SystemInfo& systemInfo() override { return sdlSystemInfo(); }
    Gamepads& gamepads() override { return sdlGamepads(); }

private:
    DrmDisplays m_displays;
    DrmKeyboard m_keyboard;
    NoDialogs m_dialogs;
};

}  // namespace

WindowSystem& drmWindowSystem() {
    static DrmWindowSystem s;
    return s;
}

}  // namespace bro::platform
