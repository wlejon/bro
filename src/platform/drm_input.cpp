#include "platform/drm_input.h"
#include "platform/drm_seat.h"
#include "platform/evdev_keymap.h"
#include "platform/keys.h"

#include <algorithm>

#if defined(__linux__) && BRO_WITH_SEAT && BRO_HAVE_LIBINPUT
#include "util/log.h"
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace {

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

DrmInputEvent DrmInputPlatform::keyEvent(uint32_t key, bool pressed) {
    // The seat has no layout of its own yet: keys mean what they mean on
    // US-QWERTY (a client of the shell's compositor gets the raw key and
    // applies its own keymap).
    const Scancode scancode = evdevKeyToScancode(key);
    const Keycode keycode = defaultKeyFromScancode(scancode);

    auto track = [&](KeyMods bit) {
        if (pressed) modifiers_ |= bit;
        else modifiers_ &= ~bit;
    };
    if (scancode == sc::LShift || scancode == sc::RShift) track(kmod::Shift);
    else if (scancode == sc::LCtrl || scancode == sc::RCtrl) track(kmod::Ctrl);
    else if (scancode == sc::LAlt || scancode == sc::RAlt) track(kmod::Alt);
    else if (scancode == sc::LGui || scancode == sc::RGui) track(kmod::Gui);

    DrmInputEvent out;
    out.type = pressed ? DrmInputEvent::Type::KeyDown : DrmInputEvent::Type::KeyUp;
    out.rawKeycode = key;
    out.scancode = static_cast<int32_t>(scancode);
    out.keycode = static_cast<int32_t>(keycode);
    out.modifiers = modifiers_;
    return out;
}

DrmInputEvent DrmInputPlatform::pointerMotion(float dx, float dy) {
    cursorX_ = std::clamp(cursorX_ + dx, 0.0f, static_cast<float>(screenWidth_));
    cursorY_ = std::clamp(cursorY_ + dy, 0.0f, static_cast<float>(screenHeight_));

    DrmInputEvent out;
    out.type = DrmInputEvent::Type::MouseMove;
    out.x = cursorX_;
    out.y = cursorY_;
    out.dx = dx;
    out.dy = dy;
    return out;
}

DrmInputEvent DrmInputPlatform::pointerMotionAbsolute(float x, float y) {
    const float fromX = cursorX_, fromY = cursorY_;
    cursorX_ = std::clamp(x, 0.0f, static_cast<float>(screenWidth_));
    cursorY_ = std::clamp(y, 0.0f, static_cast<float>(screenHeight_));

    DrmInputEvent out;
    out.type = DrmInputEvent::Type::MouseMove;
    out.x = cursorX_;
    out.y = cursorY_;
    // The move as a delta too, as wlroots compositors derive one: a client
    // holding the pointer (a lock) sees an absolute device's motion as
    // relative motion, and a remote viewer's mouse drives a game.
    out.dx = x - fromX;
    out.dy = y - fromY;
    return out;
}

DrmInputEvent DrmInputPlatform::buttonEvent(uint32_t button, bool pressed) {
    DrmInputEvent out;
    out.type = pressed ? DrmInputEvent::Type::MouseDown : DrmInputEvent::Type::MouseUp;
    out.x = cursorX_;
    out.y = cursorY_;
    out.button = evdevButtonToMouseButton(button);
    out.rawButton = button;
    return out;
}

DrmInputEvent DrmInputPlatform::wheelEvent(float horizontal, float vertical) {
    DrmInputEvent out;
    out.type = DrmInputEvent::Type::MouseWheel;
    out.x = cursorX_;
    out.y = cursorY_;
    out.wheelDx = horizontal;
    out.wheelDy = vertical;
    return out;
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
                const uint32_t key = libinput_event_keyboard_get_key(kb);
                const bool pressed = libinput_event_keyboard_get_key_state(kb) == LIBINPUT_KEY_STATE_PRESSED;
                DrmInputEvent out = keyEvent(key, pressed);
                if (handler) handler(out);
                break;
            }
            case LIBINPUT_EVENT_POINTER_MOTION: {
                auto* pt = libinput_event_get_pointer_event(ev);
                DrmInputEvent out = pointerMotion(static_cast<float>(libinput_event_pointer_get_dx(pt)),
                                                  static_cast<float>(libinput_event_pointer_get_dy(pt)));
                if (handler) handler(out);
                break;
            }
            case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
                auto* pt = libinput_event_get_pointer_event(ev);
                double x = libinput_event_pointer_get_absolute_x_transformed(pt, screenWidth_);
                double y = libinput_event_pointer_get_absolute_y_transformed(pt, screenHeight_);
                DrmInputEvent out = pointerMotionAbsolute(static_cast<float>(x), static_cast<float>(y));
                if (handler) handler(out);
                break;
            }
            case LIBINPUT_EVENT_POINTER_BUTTON: {
                auto* pt = libinput_event_get_pointer_event(ev);
                const uint32_t btn = libinput_event_pointer_get_button(pt);
                const bool pressed =
                    libinput_event_pointer_get_button_state(pt) == LIBINPUT_BUTTON_STATE_PRESSED;
                DrmInputEvent out = buttonEvent(btn, pressed);
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
                DrmInputEvent out = wheelEvent(static_cast<float>(h), static_cast<float>(v));
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
