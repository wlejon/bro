#include "dom/event.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace bro::dom {

static double currentTimeMs() {
#ifdef _WIN32
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
#endif
}

// ---------------------------------------------------------------------------
// Event
// ---------------------------------------------------------------------------

Event::Event(const std::string& type, bool bubbles, bool cancelable)
    : type_(type)
    , bubbles_(bubbles)
    , cancelable_(cancelable)
    , timeStamp_(currentTimeMs())
{
}

void Event::preventDefault() {
    if (cancelable_) {
        defaultPrevented_ = true;
    }
}

void Event::stopPropagation() {
    propagationStopped_ = true;
}

// ---------------------------------------------------------------------------
// MouseEvent
// ---------------------------------------------------------------------------

MouseEvent::MouseEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
}

// ---------------------------------------------------------------------------
// KeyboardEvent
// ---------------------------------------------------------------------------

KeyboardEvent::KeyboardEvent(const std::string& type, bool bubbles, bool cancelable)
    : Event(type, bubbles, cancelable)
{
}

} // namespace bro::dom
