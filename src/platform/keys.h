#pragma once
// bro's key model, one for every platform backend.
//
// - A Scancode names a physical key position: the USB HID usage ID on the
//   keyboard/keypad page (0x07), extended past 0xFF for the consumer-page keys
//   (media, application control) bro also handles. HID is what Win32 scan
//   codes, evdev KEY_* codes, macOS virtual key codes and DOM's
//   KeyboardEvent.code all map onto, so every backend has a fixed table to it.
// - A Keycode names what the key means in the active layout: the Unicode code
//   point it types unshifted (lower-case for letters), or, for a key that types
//   nothing, its scancode with kScancodeMask set. A few keys with neither a
//   character nor a HID position use kExtendedMask.
// - KeyMods is the modifier state as left/right bits.
//
// The numbering is SDL3's (SDL_Scancode, SDL_Keycode, SDL_Keymod are the same
// HID-based model), so the SDL backend passes its values through unchanged and
// asserts the equivalence at compile time. Nothing here depends on SDL.

#include <cstdint>

namespace bro::platform {

using Scancode = int32_t;
using Keycode = int32_t;
using KeyMods = uint16_t;

inline constexpr Keycode kScancodeMask = 1 << 30;
inline constexpr Keycode kExtendedMask = 1 << 29;

/// The Keycode of a key that types no character.
constexpr Keycode keycodeFromScancode(Scancode s) { return s | kScancodeMask; }

/// Physical keys: USB HID usage IDs (keyboard page), consumer keys above 256.
namespace sc {
inline constexpr Scancode Unknown = 0;
inline constexpr Scancode A = 4, B = 5, C = 6, D = 7, E = 8, F = 9, G = 10, H = 11, I = 12,
                          J = 13, K = 14, L = 15, M = 16, N = 17, O = 18, P = 19, Q = 20,
                          R = 21, S = 22, T = 23, U = 24, V = 25, W = 26, X = 27, Y = 28, Z = 29;
inline constexpr Scancode Digit1 = 30, Digit2 = 31, Digit3 = 32, Digit4 = 33, Digit5 = 34,
                          Digit6 = 35, Digit7 = 36, Digit8 = 37, Digit9 = 38, Digit0 = 39;
inline constexpr Scancode Return = 40, Escape = 41, Backspace = 42, Tab = 43, Space = 44;
inline constexpr Scancode Minus = 45, Equals = 46, LeftBracket = 47, RightBracket = 48,
                          Backslash = 49, NonUsHash = 50, Semicolon = 51, Apostrophe = 52,
                          Grave = 53, Comma = 54, Period = 55, Slash = 56;
inline constexpr Scancode CapsLock = 57;
inline constexpr Scancode F1 = 58, F2 = 59, F3 = 60, F4 = 61, F5 = 62, F6 = 63, F7 = 64,
                          F8 = 65, F9 = 66, F10 = 67, F11 = 68, F12 = 69;
inline constexpr Scancode PrintScreen = 70, ScrollLock = 71, Pause = 72, Insert = 73,
                          Home = 74, PageUp = 75, Delete = 76, End = 77, PageDown = 78,
                          Right = 79, Left = 80, Down = 81, Up = 82;
inline constexpr Scancode NumLockClear = 83, KpDivide = 84, KpMultiply = 85, KpMinus = 86,
                          KpPlus = 87, KpEnter = 88, Kp1 = 89, Kp2 = 90, Kp3 = 91, Kp4 = 92,
                          Kp5 = 93, Kp6 = 94, Kp7 = 95, Kp8 = 96, Kp9 = 97, Kp0 = 98,
                          KpPeriod = 99;
inline constexpr Scancode NonUsBackslash = 100, Application = 101, Power = 102, KpEquals = 103;
inline constexpr Scancode F13 = 104, F14 = 105, F15 = 106, F16 = 107, F17 = 108, F18 = 109,
                          F19 = 110, F20 = 111, F21 = 112, F22 = 113, F23 = 114, F24 = 115;
inline constexpr Scancode Execute = 116, Help = 117, Menu = 118, Select = 119, Stop = 120,
                          Again = 121, Undo = 122, Cut = 123, Copy = 124, Paste = 125,
                          Find = 126, Mute = 127, VolumeUp = 128, VolumeDown = 129;
inline constexpr Scancode KpComma = 133, KpEqualsAs400 = 134;
inline constexpr Scancode International1 = 135, International2 = 136, International3 = 137,
                          International4 = 138, International5 = 139, International6 = 140,
                          International7 = 141, International8 = 142, International9 = 143;
inline constexpr Scancode Lang1 = 144, Lang2 = 145, Lang3 = 146, Lang4 = 147, Lang5 = 148,
                          Lang6 = 149, Lang7 = 150, Lang8 = 151, Lang9 = 152;
inline constexpr Scancode AltErase = 153, SysReq = 154, Cancel = 155, Clear = 156, Prior = 157,
                          Return2 = 158, Separator = 159, Out = 160, Oper = 161,
                          ClearAgain = 162, CrSel = 163, ExSel = 164;
inline constexpr Scancode Kp00 = 176, Kp000 = 177, ThousandsSeparator = 178,
                          DecimalSeparator = 179, CurrencyUnit = 180, CurrencySubunit = 181,
                          KpLeftParen = 182, KpRightParen = 183, KpLeftBrace = 184,
                          KpRightBrace = 185, KpTab = 186, KpBackspace = 187, KpA = 188,
                          KpB = 189, KpC = 190, KpD = 191, KpE = 192, KpF = 193, KpXor = 194,
                          KpPower = 195, KpPercent = 196, KpLess = 197, KpGreater = 198,
                          KpAmpersand = 199, KpDblAmpersand = 200, KpVerticalBar = 201,
                          KpDblVerticalBar = 202, KpColon = 203, KpHash = 204, KpSpace = 205,
                          KpAt = 206, KpExclam = 207, KpMemStore = 208, KpMemRecall = 209,
                          KpMemClear = 210, KpMemAdd = 211, KpMemSubtract = 212,
                          KpMemMultiply = 213, KpMemDivide = 214, KpPlusMinus = 215,
                          KpClear = 216, KpClearEntry = 217, KpBinary = 218, KpOctal = 219,
                          KpDecimal = 220, KpHexadecimal = 221;
inline constexpr Scancode LCtrl = 224, LShift = 225, LAlt = 226, LGui = 227,
                          RCtrl = 228, RShift = 229, RAlt = 230, RGui = 231;
// Past the HID keyboard page: AltGr-style mode switch, then consumer keys.
inline constexpr Scancode Mode = 257, Sleep = 258, Wake = 259, ChannelIncrement = 260,
                          ChannelDecrement = 261, MediaPlay = 262, MediaPause = 263,
                          MediaRecord = 264, MediaFastForward = 265, MediaRewind = 266,
                          MediaNextTrack = 267, MediaPreviousTrack = 268, MediaStop = 269,
                          MediaEject = 270, MediaPlayPause = 271, MediaSelect = 272;
inline constexpr Scancode AcNew = 273, AcOpen = 274, AcClose = 275, AcExit = 276, AcSave = 277,
                          AcPrint = 278, AcProperties = 279, AcSearch = 280, AcHome = 281,
                          AcBack = 282, AcForward = 283, AcStop = 284, AcRefresh = 285,
                          AcBookmarks = 286;
inline constexpr Scancode SoftLeft = 287, SoftRight = 288, Call = 289, EndCall = 290;
/// One past the largest scancode any backend reports.
inline constexpr Scancode Count = 512;
}  // namespace sc

/// Layout keys: code points for the keys that type, scancode-derived for the rest.
namespace kc {
inline constexpr Keycode Unknown = 0;
inline constexpr Keycode Return = '\r', Escape = 0x1b, Backspace = '\b', Tab = '\t', Space = ' ';
inline constexpr Keycode Exclaim = '!', DblApostrophe = '"', Hash = '#', Dollar = '$',
                         Percent = '%', Ampersand = '&', Apostrophe = '\'', LeftParen = '(',
                         RightParen = ')', Asterisk = '*', Plus = '+', Comma = ',', Minus = '-',
                         Period = '.', Slash = '/';
inline constexpr Keycode Digit0 = '0', Digit1 = '1', Digit2 = '2', Digit3 = '3', Digit4 = '4',
                         Digit5 = '5', Digit6 = '6', Digit7 = '7', Digit8 = '8', Digit9 = '9';
inline constexpr Keycode Colon = ':', Semicolon = ';', Less = '<', Equals = '=', Greater = '>',
                         Question = '?', At = '@', LeftBracket = '[', Backslash = '\\',
                         RightBracket = ']', Caret = '^', Underscore = '_', Grave = '`';
inline constexpr Keycode A = 'a', B = 'b', C = 'c', D = 'd', E = 'e', F = 'f', G = 'g', H = 'h',
                         I = 'i', J = 'j', K = 'k', L = 'l', M = 'm', N = 'n', O = 'o', P = 'p',
                         Q = 'q', R = 'r', S = 's', T = 't', U = 'u', V = 'v', W = 'w', X = 'x',
                         Y = 'y', Z = 'z';
inline constexpr Keycode LeftBrace = '{', Pipe = '|', RightBrace = '}', Tilde = '~',
                         Delete = 0x7f, PlusMinus = 0xb1;

inline constexpr Keycode CapsLock = keycodeFromScancode(sc::CapsLock);
inline constexpr Keycode F1 = keycodeFromScancode(sc::F1), F2 = keycodeFromScancode(sc::F2),
                         F3 = keycodeFromScancode(sc::F3), F4 = keycodeFromScancode(sc::F4),
                         F5 = keycodeFromScancode(sc::F5), F6 = keycodeFromScancode(sc::F6),
                         F7 = keycodeFromScancode(sc::F7), F8 = keycodeFromScancode(sc::F8),
                         F9 = keycodeFromScancode(sc::F9), F10 = keycodeFromScancode(sc::F10),
                         F11 = keycodeFromScancode(sc::F11), F12 = keycodeFromScancode(sc::F12),
                         F13 = keycodeFromScancode(sc::F13), F14 = keycodeFromScancode(sc::F14),
                         F15 = keycodeFromScancode(sc::F15), F16 = keycodeFromScancode(sc::F16),
                         F17 = keycodeFromScancode(sc::F17), F18 = keycodeFromScancode(sc::F18),
                         F19 = keycodeFromScancode(sc::F19), F20 = keycodeFromScancode(sc::F20),
                         F21 = keycodeFromScancode(sc::F21), F22 = keycodeFromScancode(sc::F22),
                         F23 = keycodeFromScancode(sc::F23), F24 = keycodeFromScancode(sc::F24);
inline constexpr Keycode PrintScreen = keycodeFromScancode(sc::PrintScreen),
                         ScrollLock = keycodeFromScancode(sc::ScrollLock),
                         Pause = keycodeFromScancode(sc::Pause),
                         Insert = keycodeFromScancode(sc::Insert),
                         Home = keycodeFromScancode(sc::Home),
                         PageUp = keycodeFromScancode(sc::PageUp),
                         End = keycodeFromScancode(sc::End),
                         PageDown = keycodeFromScancode(sc::PageDown),
                         Right = keycodeFromScancode(sc::Right),
                         Left = keycodeFromScancode(sc::Left),
                         Down = keycodeFromScancode(sc::Down),
                         Up = keycodeFromScancode(sc::Up);
inline constexpr Keycode NumLockClear = keycodeFromScancode(sc::NumLockClear),
                         KpDivide = keycodeFromScancode(sc::KpDivide),
                         KpMultiply = keycodeFromScancode(sc::KpMultiply),
                         KpMinus = keycodeFromScancode(sc::KpMinus),
                         KpPlus = keycodeFromScancode(sc::KpPlus),
                         KpEnter = keycodeFromScancode(sc::KpEnter),
                         Kp1 = keycodeFromScancode(sc::Kp1), Kp2 = keycodeFromScancode(sc::Kp2),
                         Kp3 = keycodeFromScancode(sc::Kp3), Kp4 = keycodeFromScancode(sc::Kp4),
                         Kp5 = keycodeFromScancode(sc::Kp5), Kp6 = keycodeFromScancode(sc::Kp6),
                         Kp7 = keycodeFromScancode(sc::Kp7), Kp8 = keycodeFromScancode(sc::Kp8),
                         Kp9 = keycodeFromScancode(sc::Kp9), Kp0 = keycodeFromScancode(sc::Kp0),
                         KpPeriod = keycodeFromScancode(sc::KpPeriod),
                         KpEquals = keycodeFromScancode(sc::KpEquals),
                         KpComma = keycodeFromScancode(sc::KpComma);
inline constexpr Keycode Application = keycodeFromScancode(sc::Application),
                         Power = keycodeFromScancode(sc::Power),
                         Menu = keycodeFromScancode(sc::Menu),
                         Mute = keycodeFromScancode(sc::Mute),
                         VolumeUp = keycodeFromScancode(sc::VolumeUp),
                         VolumeDown = keycodeFromScancode(sc::VolumeDown);
inline constexpr Keycode LCtrl = keycodeFromScancode(sc::LCtrl),
                         LShift = keycodeFromScancode(sc::LShift),
                         LAlt = keycodeFromScancode(sc::LAlt),
                         LGui = keycodeFromScancode(sc::LGui),
                         RCtrl = keycodeFromScancode(sc::RCtrl),
                         RShift = keycodeFromScancode(sc::RShift),
                         RAlt = keycodeFromScancode(sc::RAlt),
                         RGui = keycodeFromScancode(sc::RGui),
                         Mode = keycodeFromScancode(sc::Mode);
inline constexpr Keycode MediaPlay = keycodeFromScancode(sc::MediaPlay),
                         MediaPause = keycodeFromScancode(sc::MediaPause),
                         MediaRecord = keycodeFromScancode(sc::MediaRecord),
                         MediaFastForward = keycodeFromScancode(sc::MediaFastForward),
                         MediaRewind = keycodeFromScancode(sc::MediaRewind),
                         MediaNextTrack = keycodeFromScancode(sc::MediaNextTrack),
                         MediaPreviousTrack = keycodeFromScancode(sc::MediaPreviousTrack),
                         MediaStop = keycodeFromScancode(sc::MediaStop),
                         MediaEject = keycodeFromScancode(sc::MediaEject),
                         MediaPlayPause = keycodeFromScancode(sc::MediaPlayPause);
// Keys with neither a character nor a HID position of their own.
inline constexpr Keycode LeftTab = kExtendedMask | 1, Level5Shift = kExtendedMask | 2,
                         MultiKeyCompose = kExtendedMask | 3, LMeta = kExtendedMask | 4,
                         RMeta = kExtendedMask | 5, LHyper = kExtendedMask | 6,
                         RHyper = kExtendedMask | 7;
}  // namespace kc

/// Modifier bits.
namespace kmod {
inline constexpr KeyMods None = 0x0000;
inline constexpr KeyMods LShift = 0x0001, RShift = 0x0002, Level5 = 0x0004;
inline constexpr KeyMods LCtrl = 0x0040, RCtrl = 0x0080, LAlt = 0x0100, RAlt = 0x0200;
inline constexpr KeyMods LGui = 0x0400, RGui = 0x0800;
inline constexpr KeyMods Num = 0x1000, Caps = 0x2000, Mode = 0x4000, Scroll = 0x8000;
inline constexpr KeyMods Ctrl = LCtrl | RCtrl, Shift = LShift | RShift, Alt = LAlt | RAlt,
                         Gui = LGui | RGui;
}  // namespace kmod

/// The US-QWERTY meaning of a key: the Keycode a scancode has with no layout
/// loaded (the DRM path and remote input use it; a desktop backend asks its
/// layout instead, through Keyboard). Shift and Caps pick the shifted
/// character of a typing key; every other key ignores `mods`.
Keycode defaultKeyFromScancode(Scancode scancode, KeyMods mods = kmod::None);

/// The inverse: the key that types `key` on US-QWERTY, and in `mods` (when
/// given) the modifiers that typing it needs (kmod::Shift or none).
Scancode defaultScancodeFromKey(Keycode key, KeyMods* mods = nullptr);

/// True when `mod` holds the platform's primary shortcut modifier (copy,
/// paste, select-all, ...): Command on macOS (Command or Control there),
/// Control everywhere else.
inline bool hasPrimaryMod(int mod) {
#ifdef __APPLE__
    return (mod & (kmod::Gui | kmod::Ctrl)) != 0;
#else
    return (mod & kmod::Ctrl) != 0;
#endif
}

}  // namespace bro::platform
