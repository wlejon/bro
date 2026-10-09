#include "platform/keys.h"

#include <iterator>

namespace bro::platform {

namespace {

// Digit1 .. Slash (scancodes 30 .. 56), unshifted and shifted.
constexpr Keycode kNormalSymbols[] = {
    kc::Digit1, kc::Digit2, kc::Digit3, kc::Digit4, kc::Digit5, kc::Digit6, kc::Digit7,
    kc::Digit8, kc::Digit9, kc::Digit0, kc::Return, kc::Escape, kc::Backspace, kc::Tab,
    kc::Space, kc::Minus, kc::Equals, kc::LeftBracket, kc::RightBracket, kc::Backslash,
    kc::Hash, kc::Semicolon, kc::Apostrophe, kc::Grave, kc::Comma, kc::Period, kc::Slash,
};
constexpr Keycode kShiftedSymbols[] = {
    kc::Exclaim, kc::At, kc::Hash, kc::Dollar, kc::Percent, kc::Caret, kc::Ampersand,
    kc::Asterisk, kc::LeftParen, kc::RightParen, kc::Return, kc::Escape, kc::Backspace,
    kc::Tab, kc::Space, kc::Underscore, kc::Plus, kc::LeftBrace, kc::RightBrace, kc::Pipe,
    kc::Hash, kc::Colon, kc::DblApostrophe, kc::Tilde, kc::Less, kc::Greater, kc::Question,
};
static_assert(std::size(kNormalSymbols) == sc::Slash - sc::Digit1 + 1);
static_assert(std::size(kShiftedSymbols) == sc::Slash - sc::Digit1 + 1);

struct ExtendedKey {
    Keycode key;
    Scancode scancode;
};
constexpr ExtendedKey kExtendedKeys[] = {
    {kc::LeftTab, sc::Tab},
    {kc::MultiKeyCompose, sc::Application},  // Sun keyboards
    {kc::LMeta, sc::LGui},
    {kc::RMeta, sc::RGui},
    {kc::RHyper, sc::Application},
};

// The keys that type nothing and have a Keycode of their own: every named
// scancode from CapsLock on, except the ones whose meaning only a layout gives
// (the ISO key and the international / language keys).
bool hasNonCharacterKeycode(Scancode s) {
    return (s >= sc::CapsLock && s <= sc::KpPeriod) ||
           (s >= sc::Application && s <= sc::VolumeDown) ||
           (s >= sc::KpComma && s <= sc::KpEqualsAs400) ||
           (s >= sc::AltErase && s <= sc::ExSel) ||
           (s >= sc::Kp00 && s <= sc::KpHexadecimal) ||
           (s >= sc::LCtrl && s <= sc::RGui) ||
           (s >= sc::Mode && s <= sc::EndCall);
}

}  // namespace

Keycode defaultKeyFromScancode(Scancode scancode, KeyMods mods) {
    if (scancode < sc::A || scancode >= sc::Count) return kc::Unknown;

    if (scancode < sc::Digit1) {
        bool shifted = (mods & kmod::Shift) != 0;
#ifdef __APPLE__
        // Apple maps to upper case for either shift or caps lock.
        if (mods & kmod::Caps) shifted = true;
#else
        if (mods & kmod::Caps) shifted = !shifted;
#endif
        if (mods & kmod::Mode) return kc::Unknown;
        return static_cast<Keycode>((shifted ? 'A' : 'a') + (scancode - sc::A));
    }

    if (scancode < sc::CapsLock) {
        if (mods & kmod::Mode) return kc::Unknown;
        const bool shifted = (mods & kmod::Shift) != 0;
        return (shifted ? kShiftedSymbols : kNormalSymbols)[scancode - sc::Digit1];
    }

    if (scancode == sc::Delete) return kc::Delete;
    if (hasNonCharacterKeycode(scancode)) return keycodeFromScancode(scancode);
    return kc::Unknown;
}

Scancode defaultScancodeFromKey(Keycode key, KeyMods* mods) {
    if (mods) *mods = kmod::None;
    if (key == kc::Unknown) return sc::Unknown;

    if (key & kExtendedMask) {
        for (const ExtendedKey& e : kExtendedKeys) {
            if (e.key == key) return e.scancode;
        }
        return sc::Unknown;
    }
    if (key & kScancodeMask) return static_cast<Scancode>(key & ~kScancodeMask);

    if (key >= kc::A && key <= kc::Z) return static_cast<Scancode>(sc::A + (key - kc::A));
    if (key >= 'A' && key <= 'Z') {
        if (mods) *mods = kmod::Shift;
        return static_cast<Scancode>(sc::A + (key - 'A'));
    }
    for (size_t i = 0; i < std::size(kNormalSymbols); ++i) {
        if (key == kNormalSymbols[i]) return static_cast<Scancode>(sc::Digit1 + i);
    }
    for (size_t i = 0; i < std::size(kShiftedSymbols); ++i) {
        if (key == kShiftedSymbols[i]) {
            if (mods) *mods = kmod::Shift;
            return static_cast<Scancode>(sc::Digit1 + i);
        }
    }
    if (key == kc::Delete) return sc::Delete;
    return sc::Unknown;
}

}  // namespace bro::platform
