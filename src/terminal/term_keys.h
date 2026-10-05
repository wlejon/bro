#pragma once
// SDL key events in bropty's terms (bropty/input.h). The encoding itself --
// legacy xterm, modifyOtherKeys, the kitty keyboard protocol -- is bropty's,
// chosen by the modes the program set; this only says which key it was.

#include <bropty/input.h>

namespace bro::terminal {

// SDL_Keymod bits -> bropty modifier bits.
bropty::KeyMods translateMods(int sdlMod);

enum class KeyKind {
    None,        // nothing a terminal encodes (an unknown key)
    Functional,  // Enter, arrows, F-keys, keypad, modifier keys, ...: KeyEvent::key
    Text,        // a key that types a character: KeyEvent::codepoint (unshifted)
};

struct TranslatedKey {
    KeyKind kind = KeyKind::None;
    bropty::KeyEvent ev;
};

// One SDL key event. `scancode` (0 when unknown) supplies the shifted
// character of a text key in the active layout; without it bropty derives
// the US one.
TranslatedKey translateKey(int keycode, int scancode, int sdlMod, bropty::KeyAction action);

} // namespace bro::terminal
