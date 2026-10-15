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
    /// the keyboard themselves (DRM, the Windows desktop shell's keyboard
    /// hook) honour it; elsewhere it is ignored.
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

// ---- the desktop shell over a system compositor (Windows) -----------------
//
// A process that is the session's shell (helm as the Winlogon shell) sees
// every key before the focused application does, through a low-level
// keyboard hook (WH_KEYBOARD_LL) on a thread of its own, running only while
// the shell is on and at least one hotkey is registered. Each key goes
// through routeHotkeyKey's state machine: a lone Super tap fires its binding,
// a registered chord is swallowed (the system and the focused application
// never see its key) and its callback is queued for pumpHotkeyEvents, and
// everything else passes untouched. RegisterHotKey is not used then. While a
// grab holds the keyboard and another application has the foreground, the
// keys go to the shell's document (setShellKeySink) instead. Win+L cannot be
// had: the secure desktop takes it before any hook. Ordinary apps (the shell
// off) keep RegisterHotKey and install no hook.

/// Turns the shell's keyboard routing on or off (a no-op off Windows).
void setDesktopShellKeyboard(bool on);
/// Whether the shell's keyboard routing is on.
bool desktopShellKeyboard();

/// Receives the keys a grab holds for the shell while another application
/// has the foreground: the key's evdev code, press or release, auto-repeat,
/// and the hotkey_mod bits held after it. Runs from pumpHotkeyEvents.
using ShellKeySink = std::function<void(uint32_t evdevCode, bool down, bool repeat, uint32_t mods)>;
void setShellKeySink(ShellKeySink sink);

/// What the shell's keyboard hook does with one key event, given what
/// routeHotkeyKey made of it, whether the shell's own window has the
/// foreground (its keys then reach it the ordinary way), and whether a key
/// was swallowed since Win or Alt went down (`masked`, the hook's state,
/// which `mask` sets). Pure.
///
/// Windows reads a Win or Alt release with no other key since its press as
/// a lone tap (the Start menu, the focused window's menu bar). When the
/// shell took the keys between, or the tap itself, that release is
/// swallowed and replayed behind a key that means nothing (`replayRelease`).
struct ShellHookAction {
    bool suppress = false;       // no application sees the event
    bool mask = false;           // a key was swallowed with Win or Alt held
    bool replayRelease = false;  // this Win/Alt release is swallowed, then injected after a meaningless key
    bool forward = false;        // the shell's document gets it (setShellKeySink)
};
ShellHookAction shellHookAction(const HotkeyKey& key, const HotkeyKeyResult& routed, bool shellForeground,
                                bool masked);

} // namespace bro::platform::desktop
