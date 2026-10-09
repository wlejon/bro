#include "engine/key_mapping.h"

#include "platform/keys.h"

namespace bro::engine {

std::string sdlKeycodeToWebKey(int32_t keycode, int mod)
{
    // Special keys (with platform::kScancodeMask = 0x40000000)
    switch (keycode) {
        case platform::kc::Return:    return "Enter";
        case platform::kc::Escape:    return "Escape";
        case platform::kc::Backspace: return "Backspace";
        case platform::kc::Tab:       return "Tab";
        case platform::kc::Space:     return " ";
        case platform::kc::Delete:    return "Delete";
        case platform::kc::Insert:    return "Insert";
        case platform::kc::Home:      return "Home";
        case platform::kc::End:       return "End";
        case platform::kc::PageUp:    return "PageUp";
        case platform::kc::PageDown:  return "PageDown";
        case platform::kc::Right:     return "ArrowRight";
        case platform::kc::Left:      return "ArrowLeft";
        case platform::kc::Down:      return "ArrowDown";
        case platform::kc::Up:        return "ArrowUp";
        case platform::kc::F1:  return "F1";  case platform::kc::F2:  return "F2";
        case platform::kc::F3:  return "F3";  case platform::kc::F4:  return "F4";
        case platform::kc::F5:  return "F5";  case platform::kc::F6:  return "F6";
        case platform::kc::F7:  return "F7";  case platform::kc::F8:  return "F8";
        case platform::kc::F9:  return "F9";  case platform::kc::F10: return "F10";
        case platform::kc::F11: return "F11"; case platform::kc::F12: return "F12";
        case platform::kc::LShift: case platform::kc::RShift: return "Shift";
        case platform::kc::LCtrl:  case platform::kc::RCtrl:  return "Control";
        case platform::kc::LAlt:   case platform::kc::RAlt:   return "Alt";
        case platform::kc::LGui:   case platform::kc::RGui:   return "Meta";
        case platform::kc::CapsLock:   return "CapsLock";
        case platform::kc::NumLockClear: return "NumLock";
        case platform::kc::ScrollLock: return "ScrollLock";
        case platform::kc::Pause:     return "Pause";
        case platform::kc::PrintScreen: return "PrintScreen";
        case platform::kc::Menu:      return "ContextMenu";
        case platform::kc::Application: return "ContextMenu";
        // Keypad: the character it types (the Mac keypad has no NumLock, and
        // key events carry these keycodes whatever the NumLock state).
        case platform::kc::KpEnter:    return "Enter";
        case platform::kc::KpDivide:   return "/";
        case platform::kc::KpMultiply: return "*";
        case platform::kc::KpMinus:    return "-";
        case platform::kc::KpPlus:     return "+";
        case platform::kc::KpPeriod:   return ".";
        case platform::kc::KpEquals:   return "=";
        case platform::kc::Kp0: return "0";
        case platform::kc::Kp1: return "1"; case platform::kc::Kp2: return "2";
        case platform::kc::Kp3: return "3"; case platform::kc::Kp4: return "4";
        case platform::kc::Kp5: return "5"; case platform::kc::Kp6: return "6";
        case platform::kc::Kp7: return "7"; case platform::kc::Kp8: return "8";
        case platform::kc::Kp9: return "9";
        default: break;
    }

    // Printable ASCII characters
    if (keycode >= 'a' && keycode <= 'z') {
        bool shift = (mod & platform::kmod::Shift) != 0;
        char c = shift ? (char)(keycode - 32) : (char)keycode;
        return std::string(1, c);
    }
    if (keycode >= '0' && keycode <= '9') {
        // Handle shift+digit for symbols
        if (mod & platform::kmod::Shift) {
            const char* symbols = ")!@#$%^&*(";
            return std::string(1, symbols[keycode - '0']);
        }
        return std::string(1, (char)keycode);
    }

    // Punctuation
    switch (keycode) {
        case platform::kc::Minus:         return (mod & platform::kmod::Shift) ? "_" : "-";
        case platform::kc::Equals:        return (mod & platform::kmod::Shift) ? "+" : "=";
        case platform::kc::LeftBracket:   return (mod & platform::kmod::Shift) ? "{" : "[";
        case platform::kc::RightBracket:  return (mod & platform::kmod::Shift) ? "}" : "]";
        case platform::kc::Backslash:     return (mod & platform::kmod::Shift) ? "|" : "\\";
        case platform::kc::Semicolon:     return (mod & platform::kmod::Shift) ? ":" : ";";
        case platform::kc::Apostrophe:    return (mod & platform::kmod::Shift) ? "\"" : "'";
        case platform::kc::Grave:         return (mod & platform::kmod::Shift) ? "~" : "`";
        case platform::kc::Comma:         return (mod & platform::kmod::Shift) ? "<" : ",";
        case platform::kc::Period:        return (mod & platform::kmod::Shift) ? ">" : ".";
        case platform::kc::Slash:         return (mod & platform::kmod::Shift) ? "?" : "/";
        default: break;
    }

    // Fallback: return the numeric keycode as string
    return std::to_string(keycode);
}

std::string sdlScancodeToWebCode(int32_t scancode)
{
    // Letters (platform::sc::A=4 through platform::sc::Z=29)
    if (scancode >= 4 && scancode <= 29) {
        char c = 'A' + (char)(scancode - 4);
        return std::string("Key") + c;
    }
    // Digits (platform::sc::Digit1=30 through platform::sc::Digit0=39)
    if (scancode >= 30 && scancode <= 39) {
        char c = (scancode == 39) ? '0' : (char)('1' + (scancode - 30));
        return std::string("Digit") + c;
    }

    switch (scancode) {
        case 40: return "Enter";
        case 41: return "Escape";
        case 42: return "Backspace";
        case 43: return "Tab";
        case 44: return "Space";
        case 45: return "Minus";
        case 46: return "Equal";
        case 47: return "BracketLeft";
        case 48: return "BracketRight";
        case 49: return "Backslash";
        case 50: return "Backslash";  // ISO keyboards: the key left of Enter
        case 51: return "Semicolon";
        case 52: return "Quote";
        case 53: return "Backquote";
        case 54: return "Comma";
        case 55: return "Period";
        case 56: return "Slash";
        case 57: return "CapsLock";
        case 58: return "F1";  case 59: return "F2";
        case 60: return "F3";  case 61: return "F4";
        case 62: return "F5";  case 63: return "F6";
        case 64: return "F7";  case 65: return "F8";
        case 66: return "F9";  case 67: return "F10";
        case 68: return "F11"; case 69: return "F12";
        case 70: return "PrintScreen";
        case 71: return "ScrollLock";
        case 72: return "Pause";
        case 73: return "Insert";
        case 74: return "Home";
        case 75: return "PageUp";
        case 76: return "Delete";
        case 77: return "End";
        case 78: return "PageDown";
        case 79: return "ArrowRight";
        case 80: return "ArrowLeft";
        case 81: return "ArrowDown";
        case 82: return "ArrowUp";
        case 83: return "NumLock";
        case 84: return "NumpadDivide";
        case 85: return "NumpadMultiply";
        case 86: return "NumpadSubtract";
        case 87: return "NumpadAdd";
        case 88: return "NumpadEnter";
        case 99: return "NumpadDecimal";
        case 100: return "IntlBackslash";
        case 101: return "ContextMenu";
        case 103: return "NumpadEqual";
        // Modifiers, in SDL's (= USB HID's) order: LCTRL, LSHIFT, LALT, LGUI,
        // then the right-hand four. GUI is the Windows key / Command (⌘),
        // which the web calls Meta.
        case 224: return "ControlLeft";
        case 225: return "ShiftLeft";
        case 226: return "AltLeft";
        case 227: return "MetaLeft";
        case 228: return "ControlRight";
        case 229: return "ShiftRight";
        case 230: return "AltRight";
        case 231: return "MetaRight";
        default: break;
    }
    // Numpad digits (platform::sc::Kp1=89 through platform::sc::Kp0=98)
    if (scancode >= 89 && scancode <= 98) {
        char c = (scancode == 98) ? '0' : (char)('1' + (scancode - 89));
        return std::string("Numpad") + c;
    }
    // F13..F24 (platform::sc::F13=104 through platform::sc::F24=115)
    if (scancode >= 104 && scancode <= 115) {
        return "F" + std::to_string(13 + (scancode - 104));
    }
    return "Unknown" + std::to_string(scancode);
}

} // namespace bro::engine
