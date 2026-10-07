#include "platform/drm_input.h"
#include "platform/drm_seat.h"

#include <algorithm>

#if defined(__linux__) && BRO_WITH_SEAT && BRO_HAVE_LIBINPUT
#include <unistd.h>
#include <linux/input-event-codes.h>

namespace {

static int open_restricted(const char* path, int flags, void* user_data) {
    auto* self = static_cast<bro::platform::DrmInputPlatform*>(user_data);
    if (!self || !self->seat()) return -1;
    (void)flags;
    return self->seat()->openDevice(path);
}

static void close_restricted(int fd, void* user_data) {
    auto* self = static_cast<bro::platform::DrmInputPlatform*>(user_data);
    if (!self || !self->seat()) {
        ::close(fd);
        return;
    }
    self->seat()->closeDevice(fd);
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

                DrmInputEvent out;
                out.type = pressed ? DrmInputEvent::Type::KeyDown : DrmInputEvent::Type::KeyUp;
                out.scancode = static_cast<int32_t>(key);
                out.keycode = static_cast<int32_t>(key);
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
