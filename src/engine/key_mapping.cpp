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
        case 224: return "ShiftLeft";
        case 225: return "ShiftRight";
        case 226: return "ControlLeft";
        case 228: return "AltLeft";
        case 230: return "AltRight";
        default: break;
    }
    return "Unknown" + std::to_string(scancode);
}

} // namespace bro::engine
