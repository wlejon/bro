#include "util/main_loop_wake.h"

#include <atomic>

namespace bro::util {

namespace {
std::atomic<void (*)()> g_waker{nullptr};
}

void wakeMainLoop() {
    if (void (*waker)() = g_waker.load(std::memory_order_acquire)) waker();
}

void setMainLoopWaker(void (*waker)()) { g_waker.store(waker, std::memory_order_release); }

}  // namespace bro::util
