#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace bro::platform { class Window; }

namespace bro::platform::desktop {

/// Options of a global hotkey registration.
struct HotkeyOptions {
    /// When the chord fires, the keyboard stays with the app (the shell)
    /// until every modifier of the chord is released, that release included
    /// (hotkeyGrabActive()). An Alt+Tab switcher uses it to see the Tab
    /// presses and the Alt release that follow its chord. Hosts that route
    /// the keyboard themselves (DRM) honour it; elsewhere it is ignored.
    bool grab = false;
};

/// Callback of a hotkey; receives the id registerGlobalHotkey returned.
using HotkeyCallback = std::function<void(uint32_t id)>;

/// Registers a system-wide global hotkey. Returns a non-zero hotkey ID on success, or 0 on failure.
///
/// Accelerators are Electron-style: modifiers (Ctrl/Control, Alt/Option,
/// Shift, Super/Meta/Cmd/Win, CommandOrControl) joined by '+' with one key
/// (A-Z, 0-9, F1-F24, Space, Tab, Enter, Escape, Backspace, Delete, Insert,
/// Home, End, PageUp, PageDown, Up/Down/Left/Right, punctuation such as ','
/// and the media keys VolumeUp/VolumeDown/VolumeMute/MediaPlayPause/
/// MediaNextTrack/MediaPreviousTrack/MediaStop/PrintScreen). Modifiers with
/// no key ("Super") name a tap: the modifiers pressed and released with no
/// other key or pointer button in between, firing on the release.
uint32_t registerGlobalHotkey(
    const Window* window,
    const std::string& accelerator,
    HotkeyCallback callback,
    HotkeyOptions options = {}
);

/// Unregisters a global hotkey by its ID. Returns true if found and removed.
bool unregisterGlobalHotkey(uint32_t id);

/// Unregisters all global hotkeys.
void unregisterAllGlobalHotkeys();

/// Simulates hotkey press by accelerator string (useful for headless testing).
bool simulateGlobalHotkey(const std::string& accelerator);

/// Simulates hotkey press by ID.
bool simulateGlobalHotkeyId(uint32_t id);

/// Dispatches any queued hotkey events on the main thread.
void pumpHotkeyEvents();

// ---- key routing for hosts that own the keyboard (DRM) -------------------
//
// Such a host feeds every key event through routeHotkeyKey() before deciding
// where it goes. A key whose press matched a registered chord is consumed:
// its press (and auto-repeats, and its release) reach neither a client nor
// the app's DOM, and the chord's callback has already run. Modifier keys are
// never consumed, so clients keep a consistent modifier state.

namespace hotkey_mod {
inline constexpr uint32_t Ctrl = 1u << 0;
inline constexpr uint32_t Alt = 1u << 1;
inline constexpr uint32_t Shift = 1u << 2;
inline constexpr uint32_t Meta = 1u << 3;
}  // namespace hotkey_mod

struct HotkeyKey {
    uint32_t code = 0;         // physical key id (the scancode): pairs a release with its press
    std::string key;           // accelerator key name ("tab", "l", "f1", ","); "" for a modifier key
    uint32_t modifierKey = 0;  // the hotkey_mod bit when this key is itself a modifier
    uint32_t mods = 0;         // hotkey_mod bits held with this event
    bool down = true;
    bool repeat = false;
};

struct HotkeyKeyResult {
    bool consumed = false;  // a chord's key: deliver it to nobody
    bool fired = false;     // a callback ran
    bool grabbed = false;   // a grab holds the keyboard for this event: deliver it to the app
};

/// Builds a HotkeyKey from a key event (platform/keys.h: Keycode, Scancode,
/// KeyMods).
HotkeyKey hotkeyKeyFromKeyEvent(int32_t keycode, int32_t scancode, int32_t mods, bool down, bool repeat);

/// Builds a HotkeyKey from accelerator spellings: `key` is a key name or a
/// modifier name ("alt", "super", ...), `mods` the modifiers held
/// ("ctrl+shift"). For tests and remote input.
HotkeyKey hotkeyKeyFromNames(const std::string& key, const std::string& mods, uint32_t code,
                             bool down, bool repeat);

/// Matches one key event against the registered hotkeys and runs the
/// matching callback synchronously (on the calling thread, which must be the
/// one that owns the JS realm).
HotkeyKeyResult routeHotkeyKey(const HotkeyKey& key);

/// A pointer button was pressed: modifiers held across it are not a tap.
void cancelHotkeyTap();

/// A grab registered with HotkeyOptions::grab is holding the keyboard.
bool hotkeyGrabActive();

/// Forgets the key-routing state (held modifiers, consumed keys, grab).
void resetHotkeyKeyState();

} // namespace bro::platform::desktop
