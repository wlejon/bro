#include "terminal/term_keys.h"

#include "platform/keyboard.h"
#include "platform/keys.h"

namespace bro::terminal {

using bropty::Key;
namespace kc = platform::kc;
namespace kmod = platform::kmod;

bropty::KeyMods translateMods(int m) {
    bropty::KeyMods out = bropty::Mod_None;
    if (m & kmod::Shift) out |= bropty::Mod_Shift;
    if (m & kmod::Ctrl) out |= bropty::Mod_Ctrl;
    if (m & kmod::Alt) out |= bropty::Mod_Alt;
    if (m & kmod::Gui) out |= bropty::Mod_Super;
    if (m & kmod::Caps) out |= bropty::Mod_CapsLock;
    if (m & kmod::Num) out |= bropty::Mod_NumLock;
    return out;
}

namespace {

Key functionalKey(platform::Keycode k) {
    switch (k) {
        case kc::Escape: return Key::Escape;
        case kc::Return: return Key::Enter;
        case kc::Tab: return Key::Tab;
        case kc::Backspace: return Key::Backspace;
        case kc::Insert: return Key::Insert;
        case kc::Delete: return Key::Delete;
        case kc::Left: return Key::Left;
        case kc::Right: return Key::Right;
        case kc::Up: return Key::Up;
        case kc::Down: return Key::Down;
        case kc::PageUp: return Key::PageUp;
        case kc::PageDown: return Key::PageDown;
        case kc::Home: return Key::Home;
        case kc::End: return Key::End;
        case kc::CapsLock: return Key::CapsLock;
        case kc::ScrollLock: return Key::ScrollLock;
        case kc::NumLockClear: return Key::NumLock;
        case kc::PrintScreen: return Key::PrintScreen;
        case kc::Pause: return Key::Pause;
        case kc::Application:
        case kc::Menu: return Key::Menu;
        case kc::Kp0: return Key::Kp0;
        case kc::Kp1: return Key::Kp1;
        case kc::Kp2: return Key::Kp2;
        case kc::Kp3: return Key::Kp3;
        case kc::Kp4: return Key::Kp4;
        case kc::Kp5: return Key::Kp5;
        case kc::Kp6: return Key::Kp6;
        case kc::Kp7: return Key::Kp7;
        case kc::Kp8: return Key::Kp8;
        case kc::Kp9: return Key::Kp9;
        case kc::KpPeriod: return Key::KpDecimal;
        case kc::KpDivide: return Key::KpDivide;
        case kc::KpMultiply: return Key::KpMultiply;
        case kc::KpMinus: return Key::KpSubtract;
        case kc::KpPlus: return Key::KpAdd;
        case kc::KpEnter: return Key::KpEnter;
        case kc::KpEquals: return Key::KpEqual;
        case kc::KpComma: return Key::KpSeparator;
        case kc::LShift: return Key::LeftShift;
        case kc::LCtrl: return Key::LeftControl;
        case kc::LAlt: return Key::LeftAlt;
        case kc::LGui: return Key::LeftSuper;
        case kc::RShift: return Key::RightShift;
        case kc::RCtrl: return Key::RightControl;
        case kc::RAlt: return Key::RightAlt;
        case kc::RGui: return Key::RightSuper;
        case kc::Mode: return Key::IsoLevel3Shift;
        case kc::MediaPlay: return Key::MediaPlay;
        case kc::MediaPause: return Key::MediaPause;
        case kc::MediaPlayPause: return Key::MediaPlayPause;
        case kc::MediaStop: return Key::MediaStop;
        case kc::MediaFastForward: return Key::MediaFastForward;
        case kc::MediaRewind: return Key::MediaRewind;
        case kc::MediaNextTrack: return Key::MediaTrackNext;
        case kc::MediaPreviousTrack: return Key::MediaTrackPrevious;
        case kc::MediaRecord: return Key::MediaRecord;
        case kc::VolumeDown: return Key::LowerVolume;
        case kc::VolumeUp: return Key::RaiseVolume;
        case kc::Mute: return Key::MuteVolume;
        default: break;
    }
    if (k >= kc::F1 && k <= kc::F12) return Key(uint32_t(Key::F1) + uint32_t(k - kc::F1));
    if (k >= kc::F13 && k <= kc::F24) return Key(uint32_t(Key::F13) + uint32_t(k - kc::F13));
    return Key::None;
}

} // namespace

TranslatedKey translateKey(int keycode, int scancode, int mods, bropty::KeyAction action) {
    TranslatedKey out;
    const platform::Keycode k = static_cast<platform::Keycode>(keycode);
    const Key fk = functionalKey(k);
    out.ev.mods = translateMods(mods);
    out.ev.action = action;
    if (fk != Key::None) {
        out.kind = KeyKind::Functional;
        out.ev.key = fk;
        return out;
    }
    // Printable keys: keycodes are the unshifted character of the key in the
    // active layout (Latin letters for non-Latin layouts, which is what
    // kitty's base-layout key wants too).
    if (k == 0 || (k & platform::kScancodeMask) || k > 0x10FFFF || k < 0x20 || k == 0x7F) return out;
    out.kind = KeyKind::Text;
    char32_t cp = char32_t(k);
    if (cp >= U'A' && cp <= U'Z') cp = cp - U'A' + U'a';
    out.ev.codepoint = cp;
    if (scancode > 0 && scancode < platform::sc::Count) {
        const platform::Keycode shifted = platform::keyboard().layoutKeycode(
            static_cast<platform::Scancode>(scancode), kmod::Shift);
        if (shifted != 0 && !(shifted & platform::kScancodeMask) && shifted <= 0x10FFFF && shifted >= 0x20 &&
            char32_t(shifted) != cp)
            out.ev.shifted = char32_t(shifted);
    }
    return out;
}

} // namespace bro::terminal
