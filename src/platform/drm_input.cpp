#include "platform/drm_input.h"
#include "platform/drm_seat.h"

#include <algorithm>

#if defined(__linux__) && BRO_WITH_SEAT && BRO_HAVE_LIBINPUT
#include "util/log.h"
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

namespace {

static SDL_Scancode evdevToSdlScancode(uint32_t key) {
    static const SDL_Scancode kTable[256] = {
        SDL_SCANCODE_UNKNOWN,        // 0: KEY_RESERVED
        SDL_SCANCODE_ESCAPE,         // 1: KEY_ESC
        SDL_SCANCODE_1,              // 2: KEY_1
        SDL_SCANCODE_2,              // 3: KEY_2
        SDL_SCANCODE_3,              // 4: KEY_3
        SDL_SCANCODE_4,              // 5: KEY_4
        SDL_SCANCODE_5,              // 6: KEY_5
        SDL_SCANCODE_6,              // 7: KEY_6
        SDL_SCANCODE_7,              // 8: KEY_7
        SDL_SCANCODE_8,              // 9: KEY_8
        SDL_SCANCODE_9,              // 10: KEY_9
        SDL_SCANCODE_0,              // 11: KEY_0
        SDL_SCANCODE_MINUS,          // 12: KEY_MINUS
        SDL_SCANCODE_EQUALS,         // 13: KEY_EQUAL
        SDL_SCANCODE_BACKSPACE,      // 14: KEY_BACKSPACE
        SDL_SCANCODE_TAB,            // 15: KEY_TAB
        SDL_SCANCODE_Q,              // 16: KEY_Q
        SDL_SCANCODE_W,              // 17: KEY_W
        SDL_SCANCODE_E,              // 18: KEY_E
        SDL_SCANCODE_R,              // 19: KEY_R
        SDL_SCANCODE_T,              // 20: KEY_T
        SDL_SCANCODE_Y,              // 21: KEY_Y
        SDL_SCANCODE_U,              // 22: KEY_U
        SDL_SCANCODE_I,              // 23: KEY_I
        SDL_SCANCODE_O,              // 24: KEY_O
        SDL_SCANCODE_P,              // 25: KEY_P
        SDL_SCANCODE_LEFTBRACKET,    // 26: KEY_LEFTBRACE
        SDL_SCANCODE_RIGHTBRACKET,   // 27: KEY_RIGHTBRACE
        SDL_SCANCODE_RETURN,         // 28: KEY_ENTER
        SDL_SCANCODE_LCTRL,          // 29: KEY_LEFTCTRL
        SDL_SCANCODE_A,              // 30: KEY_A
        SDL_SCANCODE_S,              // 31: KEY_S
        SDL_SCANCODE_D,              // 32: KEY_D
        SDL_SCANCODE_F,              // 33: KEY_F
        SDL_SCANCODE_G,              // 34: KEY_G
        SDL_SCANCODE_H,              // 35: KEY_H
        SDL_SCANCODE_J,              // 36: KEY_J
        SDL_SCANCODE_K,              // 37: KEY_K
        SDL_SCANCODE_L,              // 38: KEY_L
        SDL_SCANCODE_SEMICOLON,      // 39: KEY_SEMICOLON
        SDL_SCANCODE_APOSTROPHE,     // 40: KEY_APOSTROPHE
        SDL_SCANCODE_GRAVE,          // 41: KEY_GRAVE
        SDL_SCANCODE_LSHIFT,         // 42: KEY_LEFTSHIFT
        SDL_SCANCODE_BACKSLASH,      // 43: KEY_BACKSLASH
        SDL_SCANCODE_Z,              // 44: KEY_Z
        SDL_SCANCODE_X,              // 45: KEY_X
        SDL_SCANCODE_C,              // 46: KEY_C
        SDL_SCANCODE_V,              // 47: KEY_V
        SDL_SCANCODE_B,              // 48: KEY_B
        SDL_SCANCODE_N,              // 49: KEY_N
        SDL_SCANCODE_M,              // 50: KEY_M
        SDL_SCANCODE_COMMA,          // 51: KEY_COMMA
        SDL_SCANCODE_PERIOD,         // 52: KEY_DOT
        SDL_SCANCODE_SLASH,          // 53: KEY_SLASH
        SDL_SCANCODE_RSHIFT,         // 54: KEY_RIGHTSHIFT
        SDL_SCANCODE_KP_MULTIPLY,    // 55: KEY_KPASTERISK
        SDL_SCANCODE_LALT,           // 56: KEY_LEFTALT
        SDL_SCANCODE_SPACE,          // 57: KEY_SPACE
        SDL_SCANCODE_CAPSLOCK,       // 58: KEY_CAPSLOCK
        SDL_SCANCODE_F1,             // 59: KEY_F1
        SDL_SCANCODE_F2,             // 60: KEY_F2
        SDL_SCANCODE_F3,             // 61: KEY_F3
        SDL_SCANCODE_F4,             // 62: KEY_F4
        SDL_SCANCODE_F5,             // 63: KEY_F5
        SDL_SCANCODE_F6,             // 64: KEY_F6
        SDL_SCANCODE_F7,             // 65: KEY_F7
        SDL_SCANCODE_F8,             // 66: KEY_F8
        SDL_SCANCODE_F9,             // 67: KEY_F9
        SDL_SCANCODE_F10,            // 68: KEY_F10
        SDL_SCANCODE_NUMLOCKCLEAR,   // 69: KEY_NUMLOCK
        SDL_SCANCODE_SCROLLLOCK,     // 70: KEY_SCROLLLOCK
        SDL_SCANCODE_KP_7,           // 71: KEY_KP7
        SDL_SCANCODE_KP_8,           // 72: KEY_KP8
        SDL_SCANCODE_KP_9,           // 73: KEY_KP9
        SDL_SCANCODE_KP_MINUS,       // 74: KEY_KPMINUS
        SDL_SCANCODE_KP_4,           // 75: KEY_KP4
        SDL_SCANCODE_KP_5,           // 76: KEY_KP5
        SDL_SCANCODE_KP_6,           // 77: KEY_KP6
        SDL_SCANCODE_KP_PLUS,        // 78: KEY_KPPLUS
        SDL_SCANCODE_KP_1,           // 79: KEY_KP1
        SDL_SCANCODE_KP_2,           // 80: KEY_KP2
        SDL_SCANCODE_KP_3,           // 81: KEY_KP3
        SDL_SCANCODE_KP_0,           // 82: KEY_KP0
        SDL_SCANCODE_KP_PERIOD,      // 83: KEY_KPDOT
        SDL_SCANCODE_UNKNOWN,        // 84
        SDL_SCANCODE_LANG5,          // 85: KEY_ZENKAKUHANKAKU
        SDL_SCANCODE_NONUSBACKSLASH, // 86: KEY_102ND
        SDL_SCANCODE_F11,            // 87: KEY_F11
        SDL_SCANCODE_F12,            // 88: KEY_F12
        SDL_SCANCODE_INTERNATIONAL1, // 89: KEY_RO
        SDL_SCANCODE_LANG3,          // 90: KEY_KATAKANA
        SDL_SCANCODE_LANG4,          // 91: KEY_HIRAGANA
        SDL_SCANCODE_INTERNATIONAL4, // 92: KEY_HENKAN
        SDL_SCANCODE_INTERNATIONAL2, // 93: KEY_KATAKANAHIRAGANA
        SDL_SCANCODE_INTERNATIONAL5, // 94: KEY_MUHENKAN
        SDL_SCANCODE_INTERNATIONAL5, // 95: KEY_KPJPCOMMA
        SDL_SCANCODE_KP_ENTER,       // 96: KEY_KPENTER
        SDL_SCANCODE_RCTRL,          // 97: KEY_RIGHTCTRL
        SDL_SCANCODE_KP_DIVIDE,      // 98: KEY_KPSLASH
        SDL_SCANCODE_SYSREQ,         // 99: KEY_SYSRQ
        SDL_SCANCODE_RALT,           // 100: KEY_RIGHTALT
        SDL_SCANCODE_UNKNOWN,        // 101: KEY_LINEFEED
        SDL_SCANCODE_HOME,           // 102: KEY_HOME
        SDL_SCANCODE_UP,             // 103: KEY_UP
        SDL_SCANCODE_PAGEUP,         // 104: KEY_PAGEUP
        SDL_SCANCODE_LEFT,           // 105: KEY_LEFT
        SDL_SCANCODE_RIGHT,          // 106: KEY_RIGHT
        SDL_SCANCODE_END,            // 107: KEY_END
        SDL_SCANCODE_DOWN,           // 108: KEY_DOWN
        SDL_SCANCODE_PAGEDOWN,       // 109: KEY_PAGEDOWN
        SDL_SCANCODE_INSERT,         // 110: KEY_INSERT
        SDL_SCANCODE_DELETE,         // 111: KEY_DELETE
        SDL_SCANCODE_UNKNOWN,        // 112: KEY_MACRO
        SDL_SCANCODE_MUTE,           // 113: KEY_MUTE
        SDL_SCANCODE_VOLUMEDOWN,     // 114: KEY_VOLUMEDOWN
        SDL_SCANCODE_VOLUMEUP,       // 115: KEY_VOLUMEUP
        SDL_SCANCODE_POWER,          // 116: KEY_POWER
        SDL_SCANCODE_KP_EQUALS,      // 117: KEY_KPEQUAL
        SDL_SCANCODE_KP_PLUSMINUS,   // 118: KEY_KPPLUSMINUS
        SDL_SCANCODE_PAUSE,          // 119: KEY_PAUSE
        SDL_SCANCODE_UNKNOWN,        // 120: KEY_SCALE
        SDL_SCANCODE_KP_COMMA,       // 121: KEY_KPCOMMA
        SDL_SCANCODE_LANG1,          // 122: KEY_HANGEUL
        SDL_SCANCODE_LANG2,          // 123: KEY_HANJA
        SDL_SCANCODE_INTERNATIONAL3, // 124: KEY_YEN
        SDL_SCANCODE_LGUI,           // 125: KEY_LEFTMETA
        SDL_SCANCODE_RGUI,           // 126: KEY_RIGHTMETA
        SDL_SCANCODE_APPLICATION,    // 127: KEY_COMPOSE
        SDL_SCANCODE_STOP,           // 128: KEY_STOP
        SDL_SCANCODE_AGAIN,          // 129: KEY_AGAIN
        SDL_SCANCODE_AC_PROPERTIES,  // 130: KEY_PROPS
        SDL_SCANCODE_UNDO,           // 131: KEY_UNDO
        SDL_SCANCODE_UNKNOWN,        // 132: KEY_FRONT
        SDL_SCANCODE_COPY,           // 133: KEY_COPY
        SDL_SCANCODE_AC_OPEN,        // 134: KEY_OPEN
        SDL_SCANCODE_PASTE,          // 135: KEY_PASTE
        SDL_SCANCODE_FIND,           // 136: KEY_FIND
        SDL_SCANCODE_CUT,            // 137: KEY_CUT
        SDL_SCANCODE_HELP,           // 138: KEY_HELP
        SDL_SCANCODE_MENU,           // 139: KEY_MENU
        SDL_SCANCODE_UNKNOWN,        // 140: KEY_CALC
        SDL_SCANCODE_UNKNOWN,        // 141: KEY_SETUP
        SDL_SCANCODE_SLEEP,          // 142: KEY_SLEEP
        SDL_SCANCODE_WAKE,           // 143: KEY_WAKEUP
        SDL_SCANCODE_UNKNOWN,        // 144: KEY_FILE
        SDL_SCANCODE_UNKNOWN,        // 145: KEY_SENDFILE
        SDL_SCANCODE_UNKNOWN,        // 146: KEY_DELETEFILE
        SDL_SCANCODE_UNKNOWN,        // 147: KEY_XFER
        SDL_SCANCODE_UNKNOWN,        // 148: KEY_PROG1
        SDL_SCANCODE_UNKNOWN,        // 149: KEY_PROG2
        SDL_SCANCODE_UNKNOWN,        // 150: KEY_WWW
        SDL_SCANCODE_UNKNOWN,        // 151: KEY_MSDOS
        SDL_SCANCODE_UNKNOWN,        // 152: KEY_COFFEE
        SDL_SCANCODE_UNKNOWN,        // 153: KEY_ROTATE_DISPLAY
        SDL_SCANCODE_UNKNOWN,        // 154: KEY_CYCLEWINDOWS
        SDL_SCANCODE_UNKNOWN,        // 155: KEY_MAIL
        SDL_SCANCODE_AC_BOOKMARKS,   // 156: KEY_BOOKMARKS
        SDL_SCANCODE_UNKNOWN,        // 157: KEY_COMPUTER
        SDL_SCANCODE_AC_BACK,        // 158: KEY_BACK
        SDL_SCANCODE_AC_FORWARD,     // 159: KEY_FORWARD
        SDL_SCANCODE_UNKNOWN,        // 160: KEY_CLOSECD
        SDL_SCANCODE_MEDIA_EJECT,    // 161: KEY_EJECTCD
        SDL_SCANCODE_MEDIA_EJECT,    // 162: KEY_EJECTCLOSECD
        SDL_SCANCODE_MEDIA_NEXT_TRACK,     // 163: KEY_NEXTSONG
        SDL_SCANCODE_MEDIA_PLAY_PAUSE,    // 164: KEY_PLAYPAUSE
        SDL_SCANCODE_MEDIA_PREVIOUS_TRACK, // 165: KEY_PREVIOUSSONG
        SDL_SCANCODE_MEDIA_STOP,          // 166: KEY_STOPCD
        SDL_SCANCODE_MEDIA_RECORD,        // 167: KEY_RECORD
        SDL_SCANCODE_MEDIA_REWIND,        // 168: KEY_REWIND
        SDL_SCANCODE_UNKNOWN,        // 169: KEY_PHONE
        SDL_SCANCODE_UNKNOWN,        // 170: KEY_ISO
        SDL_SCANCODE_UNKNOWN,        // 171: KEY_CONFIG
        SDL_SCANCODE_AC_HOME,        // 172: KEY_HOMEPAGE
        SDL_SCANCODE_AC_REFRESH,     // 173: KEY_REFRESH
        SDL_SCANCODE_AC_EXIT,        // 174: KEY_EXIT
        SDL_SCANCODE_UNKNOWN,        // 175: KEY_MOVE
        SDL_SCANCODE_UNKNOWN,        // 176: KEY_EDIT
        SDL_SCANCODE_UNKNOWN,        // 177: KEY_SCROLLUP
        SDL_SCANCODE_UNKNOWN,        // 178: KEY_SCROLLDOWN
        SDL_SCANCODE_KP_LEFTPAREN,   // 179: KEY_KPLEFTPAREN
        SDL_SCANCODE_KP_RIGHTPAREN,  // 180: KEY_KPRIGHTPAREN
        SDL_SCANCODE_AC_NEW,         // 181: KEY_NEW
        SDL_SCANCODE_AGAIN,          // 182: KEY_REDO
        SDL_SCANCODE_F13,            // 183: KEY_F13
        SDL_SCANCODE_F14,            // 184: KEY_F14
        SDL_SCANCODE_F15,            // 185: KEY_F15
        SDL_SCANCODE_F16,            // 186: KEY_F16
        SDL_SCANCODE_F17,            // 187: KEY_F17
        SDL_SCANCODE_F18,            // 188: KEY_F18
        SDL_SCANCODE_F19,            // 189: KEY_F19
        SDL_SCANCODE_F20,            // 190: KEY_F20
        SDL_SCANCODE_F21,            // 191: KEY_F21
        SDL_SCANCODE_F22,            // 192: KEY_F22
        SDL_SCANCODE_F23,            // 193: KEY_F23
        SDL_SCANCODE_F24,            // 194: KEY_F24
    };
    if (key < 256) return kTable[key];
    return SDL_SCANCODE_UNKNOWN;
}

static int open_restricted(const char* path, int flags, void* user_data) {
    auto* self = static_cast<bro::platform::DrmInputPlatform*>(user_data);
    int fd = -1;
    if (self && self->seat()) {
        fd = self->seat()->openDevice(path);
    }
    if (fd < 0) {
        fd = ::open(path, flags);
    }
    return fd < 0 ? -errno : fd;
}

static void close_restricted(int fd, void* user_data) {
    auto* self = static_cast<bro::platform::DrmInputPlatform*>(user_data);
    if (self && self->seat()) {
        self->seat()->closeDevice(fd);
    } else {
        ::close(fd);
    }
}

const struct libinput_interface kLibinputInterface = {
    open_restricted,
    close_restricted,
};

} // namespace
#endif

namespace bro::platform {

DrmInputPlatform::DrmInputPlatform() = default;

DrmInputPlatform::~DrmInputPlatform() {
    shutdown();
}

bool DrmInputPlatform::init(DrmSeatPlatform& seat, const std::string& seatName,
                            uint32_t screenWidth, uint32_t screenHeight) {
    shutdown();
    seat_ = &seat;
    screenWidth_ = screenWidth ? screenWidth : 1920;
    screenHeight_ = screenHeight ? screenHeight : 1080;
    cursorX_ = static_cast<float>(screenWidth_) / 2.0f;
    cursorY_ = static_cast<float>(screenHeight_) / 2.0f;
    modifiers_ = 0;

#if defined(__linux__) && BRO_WITH_SEAT && BRO_HAVE_LIBINPUT
    udev_ = udev_new();
    if (!udev_) return false;

    li_ = libinput_udev_create_context(&kLibinputInterface, this, udev_);
    if (!li_) {
        udev_unref(udev_);
        udev_ = nullptr;
        return false;
    }

    if (libinput_udev_assign_seat(li_, seatName.c_str()) != 0) {
        libinput_unref(li_);
        li_ = nullptr;
        udev_unref(udev_);
        udev_ = nullptr;
        return false;
    }

    valid_ = true;
    return true;
#else
    (void)seatName;
    return false;
#endif
}

void DrmInputPlatform::shutdown() {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_HAVE_LIBINPUT
    if (li_) {
        libinput_unref(li_);
        li_ = nullptr;
    }
    if (udev_) {
        udev_unref(udev_);
        udev_ = nullptr;
    }
#endif
    seat_ = nullptr;
    valid_ = false;
    modifiers_ = 0;
}

void DrmInputPlatform::setScreenSize(uint32_t width, uint32_t height) {
    if (width > 0) screenWidth_ = width;
    if (height > 0) screenHeight_ = height;
}

int DrmInputPlatform::pollFd() const {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_HAVE_LIBINPUT
    return li_ ? libinput_get_fd(li_) : -1;
#else
    return -1;
#endif
}

void DrmInputPlatform::pollEvents(const DrmInputHandler& handler) {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_HAVE_LIBINPUT
    if (!li_ || !valid_) return;

    libinput_dispatch(li_);

    while (struct libinput_event* ev = libinput_get_event(li_)) {
        const auto evType = libinput_event_get_type(ev);

        switch (evType) {
            case LIBINPUT_EVENT_KEYBOARD_KEY: {
                auto* kb = libinput_event_get_keyboard_event(ev);
                uint32_t key = libinput_event_keyboard_get_key(kb);
                auto state = libinput_event_keyboard_get_key_state(kb);
                bool pressed = (state == LIBINPUT_KEY_STATE_PRESSED);

                SDL_Scancode scancode = evdevToSdlScancode(key);
                SDL_Keycode keycode = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true);

                if (scancode == SDL_SCANCODE_LSHIFT || scancode == SDL_SCANCODE_RSHIFT) {
                    if (pressed) modifiers_ |= SDL_KMOD_SHIFT;
                    else modifiers_ &= ~SDL_KMOD_SHIFT;
                } else if (scancode == SDL_SCANCODE_LCTRL || scancode == SDL_SCANCODE_RCTRL) {
                    if (pressed) modifiers_ |= SDL_KMOD_CTRL;
                    else modifiers_ &= ~SDL_KMOD_CTRL;
                } else if (scancode == SDL_SCANCODE_LALT || scancode == SDL_SCANCODE_RALT) {
                    if (pressed) modifiers_ |= SDL_KMOD_ALT;
                    else modifiers_ &= ~SDL_KMOD_ALT;
                } else if (scancode == SDL_SCANCODE_LGUI || scancode == SDL_SCANCODE_RGUI) {
                    if (pressed) modifiers_ |= SDL_KMOD_GUI;
                    else modifiers_ &= ~SDL_KMOD_GUI;
                }

                DrmInputEvent out;
                out.type = pressed ? DrmInputEvent::Type::KeyDown : DrmInputEvent::Type::KeyUp;
                out.rawKeycode = key;
                out.scancode = static_cast<int32_t>(scancode);
                out.keycode = static_cast<int32_t>(keycode);
                out.modifiers = modifiers_;
                if (handler) handler(out);
                break;
            }
            case LIBINPUT_EVENT_POINTER_MOTION: {
                auto* pt = libinput_event_get_pointer_event(ev);
                double dx = libinput_event_pointer_get_dx(pt);
                double dy = libinput_event_pointer_get_dy(pt);

                cursorX_ = std::clamp(cursorX_ + static_cast<float>(dx), 0.0f, static_cast<float>(screenWidth_));
                cursorY_ = std::clamp(cursorY_ + static_cast<float>(dy), 0.0f, static_cast<float>(screenHeight_));

                DrmInputEvent out;
                out.type = DrmInputEvent::Type::MouseMove;
                out.x = cursorX_;
                out.y = cursorY_;
                out.dx = static_cast<float>(dx);
                out.dy = static_cast<float>(dy);
                if (handler) handler(out);
                break;
            }
            case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
                auto* pt = libinput_event_get_pointer_event(ev);
                double x = libinput_event_pointer_get_absolute_x_transformed(pt, screenWidth_);
                double y = libinput_event_pointer_get_absolute_y_transformed(pt, screenHeight_);

                cursorX_ = std::clamp(static_cast<float>(x), 0.0f, static_cast<float>(screenWidth_));
                cursorY_ = std::clamp(static_cast<float>(y), 0.0f, static_cast<float>(screenHeight_));

                DrmInputEvent out;
                out.type = DrmInputEvent::Type::MouseMove;
                out.x = cursorX_;
                out.y = cursorY_;
                if (handler) handler(out);
                break;
            }
            case LIBINPUT_EVENT_POINTER_BUTTON: {
                auto* pt = libinput_event_get_pointer_event(ev);
                uint32_t btn = libinput_event_pointer_get_button(pt);
                auto state = libinput_event_pointer_get_button_state(pt);
                bool pressed = (state == LIBINPUT_BUTTON_STATE_PRESSED);

                int buttonNum = 1;
                if (btn == BTN_RIGHT) buttonNum = 3;
                else if (btn == BTN_MIDDLE) buttonNum = 2;

                DrmInputEvent out;
                out.type = pressed ? DrmInputEvent::Type::MouseDown : DrmInputEvent::Type::MouseUp;
                out.x = cursorX_;
                out.y = cursorY_;
                out.button = buttonNum;
                out.rawButton = btn;
                if (handler) handler(out);
                break;
            }
            case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
            case LIBINPUT_EVENT_POINTER_SCROLL_FINGER: {
                auto* pt = libinput_event_get_pointer_event(ev);
                double v = 0.0, h = 0.0;
                if (libinput_event_pointer_has_axis(pt, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
                    v = libinput_event_pointer_get_scroll_value(pt, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
                if (libinput_event_pointer_has_axis(pt, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
                    h = libinput_event_pointer_get_scroll_value(pt, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);

                DrmInputEvent out;
                out.type = DrmInputEvent::Type::MouseWheel;
                out.x = cursorX_;
                out.y = cursorY_;
                out.wheelDx = static_cast<float>(h);
                out.wheelDy = static_cast<float>(v);
                if (handler) handler(out);
                break;
            }
            case LIBINPUT_EVENT_TOUCH_DOWN: {
                auto* tch = libinput_event_get_touch_event(ev);
                int slot = libinput_event_touch_get_seat_slot(tch);
                double x = libinput_event_touch_get_x_transformed(tch, screenWidth_);
                double y = libinput_event_touch_get_y_transformed(tch, screenHeight_);

                DrmInputEvent out;
                out.type = DrmInputEvent::Type::TouchDown;
                out.touchId = static_cast<uint64_t>(slot);
                out.x = static_cast<float>(x);
                out.y = static_cast<float>(y);
                if (handler) handler(out);
                break;
            }
            case LIBINPUT_EVENT_TOUCH_MOTION: {
                auto* tch = libinput_event_get_touch_event(ev);
                int slot = libinput_event_touch_get_seat_slot(tch);
                double x = libinput_event_touch_get_x_transformed(tch, screenWidth_);
                double y = libinput_event_touch_get_y_transformed(tch, screenHeight_);

                DrmInputEvent out;
                out.type = DrmInputEvent::Type::TouchMove;
                out.touchId = static_cast<uint64_t>(slot);
                out.x = static_cast<float>(x);
                out.y = static_cast<float>(y);
                if (handler) handler(out);
                break;
            }
            case LIBINPUT_EVENT_TOUCH_UP: {
                auto* tch = libinput_event_get_touch_event(ev);
                int slot = libinput_event_touch_get_seat_slot(tch);

                DrmInputEvent out;
                out.type = DrmInputEvent::Type::TouchUp;
                out.touchId = static_cast<uint64_t>(slot);
                out.x = cursorX_;
                out.y = cursorY_;
                if (handler) handler(out);
                break;
            }
            case LIBINPUT_EVENT_DEVICE_ADDED: {
                auto* dev = libinput_event_get_device(ev);
                LOG_INFO("DrmInput: device added: %s (%s)",
                         libinput_device_get_name(dev),
                         libinput_device_get_sysname(dev));
                break;
            }
            case LIBINPUT_EVENT_DEVICE_REMOVED: {
                auto* dev = libinput_event_get_device(ev);
                LOG_INFO("DrmInput: device removed: %s (%s)",
                         libinput_device_get_name(dev),
                         libinput_device_get_sysname(dev));
                break;
            }
            default:
                break;
        }

        libinput_event_destroy(ev);
    }
#else
    (void)handler;
#endif
}

} // namespace bro::platform
