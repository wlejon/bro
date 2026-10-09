#pragma once
// The keyboard as a seat has it: the active layout and the modifier state.
// Key events themselves arrive through EventLoop (or the DRM input path) in
// the key model of platform/keys.h.

#include "platform/keys.h"

namespace bro::platform {

class Keyboard {
public:
    virtual ~Keyboard() = default;

    /// The modifiers held right now.
    virtual KeyMods modState() = 0;

    /// The Keycode a key event for `scancode` carries in the active layout:
    /// the unshifted meaning, modifiers ignored.
    virtual Keycode eventKeycode(Scancode scancode) = 0;

    /// What `scancode` types in the active layout with `mods` held (Shift,
    /// Caps, AltGr pick the level): the shifted character of a text key.
    virtual Keycode layoutKeycode(Scancode scancode, KeyMods mods) = 0;

    /// The key that produces `key` in the active layout, with the modifiers
    /// it needs in `mods` when given.
    virtual Scancode scancodeFromKey(Keycode key, KeyMods* mods = nullptr) = 0;
};

/// The active WindowSystem's keyboard.
Keyboard& keyboard();

} // namespace bro::platform
