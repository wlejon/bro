#include "terminal/term_keys.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>

namespace bro::terminal {

using bropty::Key;

bropty::KeyMods translateMods(int m) {
    bropty::KeyMods out = bropty::Mod_None;
    if (m & SDL_KMOD_SHIFT) out |= bropty::Mod_Shift;
    if (m & SDL_KMOD_CTRL) out |= bropty::Mod_Ctrl;
    if (m & SDL_KMOD_ALT) out |= bropty::Mod_Alt;
    if (m & SDL_KMOD_GUI) out |= bropty::Mod_Super;
    if (m & SDL_KMOD_CAPS) out |= bropty::Mod_CapsLock;
    if (m & SDL_KMOD_NUM) out |= bropty::Mod_NumLock;
    return out;
}

namespace {

Key functionalKey(SDL_Keycode k) {
    switch (k) {
        case SDLK_ESCAPE: return Key::Escape;
        case SDLK_RETURN: return Key::Enter;
        case SDLK_TAB: return Key::Tab;
        case SDLK_BACKSPACE: return Key::Backspace;
        case SDLK_INSERT: return Key::Insert;
        case SDLK_DELETE: return Key::Delete;
        case SDLK_LEFT: return Key::Left;
        case SDLK_RIGHT: return Key::Right;
        case SDLK_UP: return Key::Up;
        case SDLK_DOWN: return Key::Down;
        case SDLK_PAGEUP: return Key::PageUp;
        case SDLK_PAGEDOWN: return Key::PageDown;
        case SDLK_HOME: return Key::Home;
        case SDLK_END: return Key::End;
        case SDLK_CAPSLOCK: return Key::CapsLock;
        case SDLK_SCROLLLOCK: return Key::ScrollLock;
        case SDLK_NUMLOCKCLEAR: return Key::NumLock;
        case SDLK_PRINTSCREEN: return Key::PrintScreen;
        case SDLK_PAUSE: return Key::Pause;
        case SDLK_APPLICATION:
        case SDLK_MENU: return Key::Menu;
        case SDLK_KP_0: return Key::Kp0;
        case SDLK_KP_1: return Key::Kp1;
        case SDLK_KP_2: return Key::Kp2;
        case SDLK_KP_3: return Key::Kp3;
        case SDLK_KP_4: return Key::Kp4;
        case SDLK_KP_5: return Key::Kp5;
        case SDLK_KP_6: return Key::Kp6;
        case SDLK_KP_7: return Key::Kp7;
        case SDLK_KP_8: return Key::Kp8;
        case SDLK_KP_9: return Key::Kp9;
        case SDLK_KP_PERIOD: return Key::KpDecimal;
        case SDLK_KP_DIVIDE: return Key::KpDivide;
        case SDLK_KP_MULTIPLY: return Key::KpMultiply;
        case SDLK_KP_MINUS: return Key::KpSubtract;
        case SDLK_KP_PLUS: return Key::KpAdd;
        case SDLK_KP_ENTER: return Key::KpEnter;
        case SDLK_KP_EQUALS: return Key::KpEqual;
        case SDLK_KP_COMMA: return Key::KpSeparator;
        case SDLK_LSHIFT: return Key::LeftShift;
        case SDLK_LCTRL: return Key::LeftControl;
        case SDLK_LALT: return Key::LeftAlt;
        case SDLK_LGUI: return Key::LeftSuper;
        case SDLK_RSHIFT: return Key::RightShift;
        case SDLK_RCTRL: return Key::RightControl;
        case SDLK_RALT: return Key::RightAlt;
        case SDLK_RGUI: return Key::RightSuper;
        case SDLK_MODE: return Key::IsoLevel3Shift;
        case SDLK_MEDIA_PLAY: return Key::MediaPlay;
        case SDLK_MEDIA_PAUSE: return Key::MediaPause;
        case SDLK_MEDIA_PLAY_PAUSE: return Key::MediaPlayPause;
        case SDLK_MEDIA_STOP: return Key::MediaStop;
        case SDLK_MEDIA_FAST_FORWARD: return Key::MediaFastForward;
        case SDLK_MEDIA_REWIND: return Key::MediaRewind;
        case SDLK_MEDIA_NEXT_TRACK: return Key::MediaTrackNext;
        case SDLK_MEDIA_PREVIOUS_TRACK: return Key::MediaTrackPrevious;
        case SDLK_MEDIA_RECORD: return Key::MediaRecord;
        case SDLK_VOLUMEDOWN: return Key::LowerVolume;
        case SDLK_VOLUMEUP: return Key::RaiseVolume;
        case SDLK_MUTE: return Key::MuteVolume;
        default: break;
    }
    if (k >= SDLK_F1 && k <= SDLK_F12) return Key(uint32_t(Key::F1) + uint32_t(k - SDLK_F1));
    if (k >= SDLK_F13 && k <= SDLK_F24) return Key(uint32_t(Key::F13) + uint32_t(k - SDLK_F13));
    return Key::None;
}

} // namespace

TranslatedKey translateKey(int keycode, int scancode, int sdlMod, bropty::KeyAction action) {
    TranslatedKey out;
    const SDL_Keycode k = static_cast<SDL_Keycode>(keycode);
    const Key fk = functionalKey(k);
    out.ev.mods = translateMods(sdlMod);
    out.ev.action = action;
    if (fk != Key::None) {
        out.kind = KeyKind::Functional;
        out.ev.key = fk;
        return out;
    }
    // Printable keys: SDL keycodes are the unshifted character of the key in
    // the active layout (Latin letters for non-Latin layouts by SDL's default
    // keycode options, which is what kitty's base-layout key wants too).
    if (k == 0 || (k & SDLK_SCANCODE_MASK) || k > 0x10FFFF || k < 0x20 || k == 0x7F) return out;
    out.kind = KeyKind::Text;
    char32_t cp = char32_t(k);
    if (cp >= U'A' && cp <= U'Z') cp = cp - U'A' + U'a';
    out.ev.codepoint = cp;
    if (scancode > 0 && scancode < SDL_SCANCODE_COUNT) {
        const SDL_Keycode shifted =
            SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(scancode), SDL_KMOD_SHIFT, false);
        if (shifted != 0 && !(shifted & SDLK_SCANCODE_MASK) && shifted <= 0x10FFFF && shifted >= 0x20 &&
            char32_t(shifted) != cp)
            out.ev.shifted = char32_t(shifted);
    }
    return out;
}

} // namespace bro::terminal
