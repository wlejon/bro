#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#if defined(__linux__) && BRO_WITH_SEAT && BRO_HAVE_LIBINPUT
#include <libinput.h>
#include <libudev.h>
#endif

namespace bro::platform {

class DrmSeatPlatform;

struct DrmInputEvent {
    enum class Type {
        None,
        KeyDown,
        KeyUp,
        MouseMove,
        MouseDown,
        MouseUp,
        MouseWheel,
        TouchDown,
        TouchMove,
        TouchUp
    } type = Type::None;

    // Keyboard
    int32_t keycode = 0;
    int32_t scancode = 0;
    int32_t modifiers = 0;
    uint32_t rawKeycode = 0; // Linux evdev KEY_* code
    bool repeat = false;

    // Pointer / mouse
    float x = 0.0f;
    float y = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    int32_t button = 0; // 1 = left, 2 = middle, 3 = right, 4 / 5 = side buttons
    uint32_t rawButton = 0; // Linux evdev BTN_* code

    // Wheel
    float wheelDx = 0.0f;
    float wheelDy = 0.0f;

    // Touch
    uint64_t touchId = 0;
    float touchPressure = 1.0f;
};

using DrmInputHandler = std::function<void(const DrmInputEvent&)>;

class DrmInputPlatform {
public:
    DrmInputPlatform();
    ~DrmInputPlatform();

    DrmInputPlatform(const DrmInputPlatform&) = delete;
    DrmInputPlatform& operator=(const DrmInputPlatform&) = delete;

    bool init(DrmSeatPlatform& seat, const std::string& seatName = "seat0",
              uint32_t screenWidth = 1920, uint32_t screenHeight = 1080);
    void shutdown();

    void setScreenSize(uint32_t width, uint32_t height);

    /// Dispatch pending libinput events to the handler
    void pollEvents(const DrmInputHandler& handler);

    /// The events pollEvents makes of libinput's, from evdev-level input:
    /// keys (KEY_*) and buttons (BTN_*), relative or absolute pointer motion
    /// in screen coordinates (the size given to init / setScreenSize), and
    /// scroll in libinput's units (degrees for a wheel, 15 per detent; +y
    /// down). They share the seat's modifier and pointer state, so input a
    /// remote viewer supplies (bro.remote) is indistinguishable from the
    /// local devices'. They need no libinput.
    DrmInputEvent keyEvent(uint32_t evdevKey, bool pressed);
    DrmInputEvent pointerMotion(float dx, float dy);
    DrmInputEvent pointerMotionAbsolute(float x, float y);
    DrmInputEvent buttonEvent(uint32_t evdevButton, bool pressed);
    DrmInputEvent wheelEvent(float horizontal, float vertical);

    bool isValid() const { return valid_; }
    int pollFd() const;

    DrmSeatPlatform* seat() { return seat_; }

private:
    DrmSeatPlatform* seat_ = nullptr;
    bool valid_ = false;
    uint32_t screenWidth_ = 1920;
    uint32_t screenHeight_ = 1080;
    float cursorX_ = 0.0f;
    float cursorY_ = 0.0f;
    int32_t modifiers_ = 0;

#if defined(__linux__) && BRO_WITH_SEAT && BRO_HAVE_LIBINPUT
    struct udev* udev_ = nullptr;
    struct libinput* li_ = nullptr;
#endif
};

} // namespace bro::platform
