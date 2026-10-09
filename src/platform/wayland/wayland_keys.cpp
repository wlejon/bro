// Keys from the seat's XKB keymap in bro's key model (platform/keys.h).
//
// A key's Keycode is what it types in the active layout, as SDL3 reports it
// with its default SDL_HINT_KEYCODE_OPTIONS ("french_numbers,latin_letters"),
// so shortcuts work the same on either backend: letter keys of a non-Latin
// layout keep their Latin letter, and a number-row key whose unshifted symbol
// is not a digit (AZERTY) reports the digit it types shifted. Keys that type
// nothing (arrows, function keys, the keypad, modifiers) report their scancode.
#include "platform/evdev_keymap.h"
#include "platform/wayland/wayland_backend.h"

#include <array>

namespace bro::platform::wl {

namespace {

// Keys whose meaning only a layout gives, beyond the typing block.
bool layoutDefined(Scancode s) {
    return (s >= sc::A && s < sc::CapsLock) || s == sc::NonUsBackslash ||
           (s >= sc::International1 && s <= sc::International9) || (s >= sc::Lang1 && s <= sc::Lang9);
}

bool isLatinLetter(uint32_t cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= 0xC0 && cp <= 0x24F && cp != 0xD7 && cp != 0xF7);
}

}  // namespace

uint32_t browlModifiers(KeyMods mods) {
    uint32_t m = 0;
    if (mods & kmod::Shift) m |= browl::modifier::Shift;
    if (mods & kmod::Ctrl) m |= browl::modifier::Ctrl;
    if (mods & kmod::Alt) m |= browl::modifier::Alt;
    if (mods & kmod::Gui) m |= browl::modifier::Logo;
    if (mods & kmod::Caps) m |= browl::modifier::CapsLock;
    if (mods & kmod::Num) m |= browl::modifier::NumLock;
    if (mods & kmod::Mode) m |= browl::modifier::AltGr;
    if (mods & kmod::Level5) m |= browl::modifier::Level5;
    return m;
}

uint32_t evdevKeyFromScancode(Scancode scancode) {
    static const std::array<uint16_t, sc::Count> table = [] {
        std::array<uint16_t, sc::Count> t{};
        // Low codes win: a scancode several evdev keys map to is the first.
        for (uint32_t key = 767; key > 0; --key) {
            const Scancode s = evdevKeyToScancode(key);
            if (s > 0 && s < sc::Count) t[static_cast<size_t>(s)] = static_cast<uint16_t>(key);
        }
        return t;
    }();
    if (scancode <= 0 || scancode >= sc::Count) return 0;
    return table[static_cast<size_t>(scancode)];
}

Keycode layoutKeycode(const browl::Keymap* keymap, Scancode scancode, KeyMods mods) {
    if (!keymap || !layoutDefined(scancode)) return defaultKeyFromScancode(scancode, mods);
    const uint32_t key = evdevKeyFromScancode(scancode);
    if (!key) return defaultKeyFromScancode(scancode, mods);

    const uint32_t bm = browlModifiers(mods);
    const uint32_t cp = browl::Keymap::keysym_to_utf32(keymap->keysym(key, bm));
    switch (cp) {
        case '\r': case '\t': case '\b': case 0x1b: case ' ': return static_cast<Keycode>(cp);
        case 0x7f: return kc::Delete;
        default: break;
    }
    if (scancode >= sc::A && scancode <= sc::Z) {
        // latin_letters: a letter key of a non-Latin layout keeps its letter.
        if (!isLatinLetter(cp)) return defaultKeyFromScancode(scancode, mods);
        return static_cast<Keycode>(cp);
    }
    if (scancode >= sc::Digit1 && scancode <= sc::Digit0 && !(mods & (kmod::Shift | kmod::Caps))) {
        // french_numbers: the digit the key types, shifted where the layout
        // puts it there.
        if (cp < '0' || cp > '9') {
            const uint32_t shifted =
                browl::Keymap::keysym_to_utf32(keymap->keysym(key, bm | browl::modifier::Shift));
            if (shifted >= '0' && shifted <= '9') return static_cast<Keycode>(shifted);
        }
    }
    if (cp < 0x20) return defaultKeyFromScancode(scancode, mods);
    return static_cast<Keycode>(cp);
}

}  // namespace bro::platform::wl
