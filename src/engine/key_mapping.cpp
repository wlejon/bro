#include "engine/key_mapping.h"

#include <SDL3/SDL_keycode.h>

namespace bro::engine {

std::string sdlKeycodeToWebKey(int32_t keycode, int mod)
{
    // Special keys (with SDLK_SCANCODE_MASK = 0x40000000)
    switch (keycode) {
        case SDLK_RETURN:    return "Enter";
        case SDLK_ESCAPE:    return "Escape";
        case SDLK_BACKSPACE: return "Backspace";
        case SDLK_TAB:       return "Tab";
        case SDLK_SPACE:     return " ";
        case SDLK_DELETE:    return "Delete";
        case SDLK_INSERT:    return "Insert";
        case SDLK_HOME:      return "Home";
        case SDLK_END:       return "End";
        case SDLK_PAGEUP:    return "PageUp";
        case SDLK_PAGEDOWN:  return "PageDown";
        case SDLK_RIGHT:     return "ArrowRight";
        case SDLK_LEFT:      return "ArrowLeft";
        case SDLK_DOWN:      return "ArrowDown";
        case SDLK_UP:        return "ArrowUp";
        case SDLK_F1:  return "F1";  case SDLK_F2:  return "F2";
        case SDLK_F3:  return "F3";  case SDLK_F4:  return "F4";
        case SDLK_F5:  return "F5";  case SDLK_F6:  return "F6";
        case SDLK_F7:  return "F7";  case SDLK_F8:  return "F8";
        case SDLK_F9:  return "F9";  case SDLK_F10: return "F10";
        case SDLK_F11: return "F11"; case SDLK_F12: return "F12";
        case SDLK_LSHIFT: case SDLK_RSHIFT: return "Shift";
        case SDLK_LCTRL:  case SDLK_RCTRL:  return "Control";
        case SDLK_LALT:   case SDLK_RALT:   return "Alt";
        case SDLK_LGUI:   case SDLK_RGUI:   return "Meta";
        case SDLK_CAPSLOCK:   return "CapsLock";
        case SDLK_NUMLOCKCLEAR: return "NumLock";
        case SDLK_SCROLLLOCK: return "ScrollLock";
        case SDLK_PAUSE:     return "Pause";
        case SDLK_PRINTSCREEN: return "PrintScreen";
        case SDLK_MENU:      return "ContextMenu";
        case SDLK_APPLICATION: return "ContextMenu";
        // Keypad: the character it types (the Mac keypad has no NumLock, and
        // SDL reports these keycodes whatever the NumLock state).
        case SDLK_KP_ENTER:    return "Enter";
        case SDLK_KP_DIVIDE:   return "/";
        case SDLK_KP_MULTIPLY: return "*";
        case SDLK_KP_MINUS:    return "-";
        case SDLK_KP_PLUS:     return "+";
        case SDLK_KP_PERIOD:   return ".";
        case SDLK_KP_EQUALS:   return "=";
        case SDLK_KP_0: return "0";
        case SDLK_KP_1: return "1"; case SDLK_KP_2: return "2";
        case SDLK_KP_3: return "3"; case SDLK_KP_4: return "4";
        case SDLK_KP_5: return "5"; case SDLK_KP_6: return "6";
        case SDLK_KP_7: return "7"; case SDLK_KP_8: return "8";
        case SDLK_KP_9: return "9";
        default: break;
    }

    // Printable ASCII characters
    if (keycode >= 'a' && keycode <= 'z') {
        bool shift = (mod & SDL_KMOD_SHIFT) != 0;
        char c = shift ? (char)(keycode - 32) : (char)keycode;
        return std::string(1, c);
    }
    if (keycode >= '0' && keycode <= '9') {
        // Handle shift+digit for symbols
        if (mod & SDL_KMOD_SHIFT) {
            const char* symbols = ")!@#$%^&*(";
            return std::string(1, symbols[keycode - '0']);
        }
        return std::string(1, (char)keycode);
    }

    // Punctuation
    switch (keycode) {
        case SDLK_MINUS:         return (mod & SDL_KMOD_SHIFT) ? "_" : "-";
        case SDLK_EQUALS:        return (mod & SDL_KMOD_SHIFT) ? "+" : "=";
        case SDLK_LEFTBRACKET:   return (mod & SDL_KMOD_SHIFT) ? "{" : "[";
        case SDLK_RIGHTBRACKET:  return (mod & SDL_KMOD_SHIFT) ? "}" : "]";
        case SDLK_BACKSLASH:     return (mod & SDL_KMOD_SHIFT) ? "|" : "\\";
        case SDLK_SEMICOLON:     return (mod & SDL_KMOD_SHIFT) ? ":" : ";";
        case SDLK_APOSTROPHE:    return (mod & SDL_KMOD_SHIFT) ? "\"" : "'";
        case SDLK_GRAVE:         return (mod & SDL_KMOD_SHIFT) ? "~" : "`";
        case SDLK_COMMA:         return (mod & SDL_KMOD_SHIFT) ? "<" : ",";
        case SDLK_PERIOD:        return (mod & SDL_KMOD_SHIFT) ? ">" : ".";
        case SDLK_SLASH:         return (mod & SDL_KMOD_SHIFT) ? "?" : "/";
        default: break;
    }

    // Fallback: return the numeric keycode as string
    return std::to_string(keycode);
}

std::string sdlScancodeToWebCode(int32_t scancode)
{
    // Letters (SDL_SCANCODE_A=4 through SDL_SCANCODE_Z=29)
    if (scancode >= 4 && scancode <= 29) {
        char c = 'A' + (char)(scancode - 4);
        return std::string("Key") + c;
    }
    // Digits (SDL_SCANCODE_1=30 through SDL_SCANCODE_0=39)
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
    // Numpad digits (SDL_SCANCODE_KP_1=89 through SDL_SCANCODE_KP_0=98)
    if (scancode >= 89 && scancode <= 98) {
        char c = (scancode == 98) ? '0' : (char)('1' + (scancode - 89));
        return std::string("Numpad") + c;
    }
    // F13..F24 (SDL_SCANCODE_F13=104 through SDL_SCANCODE_F24=115)
    if (scancode >= 104 && scancode <= 115) {
        return "F" + std::to_string(13 + (scancode - 104));
    }
    return "Unknown" + std::to_string(scancode);
}

} // namespace bro::engine
