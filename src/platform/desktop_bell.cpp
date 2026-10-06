#include "platform/desktop_bell.h"
#include "platform/desktop_platform.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace bro::platform::desktop {

namespace {
std::atomic<uint32_t> s_beepCount{0};
}

bool beep() {
    s_beepCount.fetch_add(1, std::memory_order_relaxed);

    if (isHeadless()) {
        return true;
    }

#ifdef _WIN32
    return MessageBeep(MB_OK) != FALSE;
#else
    bool played = false;
    // Try terminal bell if attached to a TTY
    if (isatty(fileno(stderr))) {
        fputc('\a', stderr);
        fflush(stderr);
        played = true;
    } else if (isatty(fileno(stdout))) {
        fputc('\a', stdout);
        fflush(stdout);
        played = true;
    }

    // Try canberra sound event asynchronously if command exists
    static bool s_hasCanberra = (std::system("which canberra-gtk-play >/dev/null 2>&1") == 0);
    if (s_hasCanberra) {
        if (std::system("canberra-gtk-play -i bell >/dev/null 2>&1 &") == 0) {
            played = true;
        }
    }

    return played || true;
#endif
}

uint32_t getHeadlessBeepCount() {
    return s_beepCount.load(std::memory_order_relaxed);
}

void resetHeadlessBeepCount() {
    s_beepCount.store(0, std::memory_order_relaxed);
}

} // namespace bro::platform::desktop
