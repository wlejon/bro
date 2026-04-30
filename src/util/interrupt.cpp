#include "util/interrupt.h"
#include "util/log.h"

#include <atomic>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

namespace bro::util {

namespace {
std::atomic<bool> g_interrupted{false};
std::atomic<bool> g_handlerInstalled{false};

#ifdef _WIN32
BOOL WINAPI consoleHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            ::bro::util::requestInterrupt();
            return TRUE;
        default:
            return FALSE;
    }
}
#else
void sigintHandler(int /*sig*/) {
    ::bro::util::requestInterrupt();
}
#endif

int jsInterruptHandler(JSRuntime* /*rt*/, void* /*opaque*/) {
    // Non-zero: break out of currently executing JS.
    return g_interrupted.load(std::memory_order_relaxed) ? 1 : 0;
}
} // namespace

bool interrupted() {
    return g_interrupted.load(std::memory_order_relaxed);
}

void requestInterrupt() {
    // Second hit: hard exit. JS is probably misbehaving.
    if (g_interrupted.exchange(true)) {
        LOG_ERROR("Second interrupt received — forcing exit.");
        std::_Exit(130);
    }
    LOG_INFO("Interrupt requested — stopping JS.");
}

void installSignalHandler() {
    if (g_handlerInstalled.exchange(true))
        return;

#ifdef _WIN32
    SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
    struct sigaction sa{};
    sa.sa_handler = sigintHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART — let syscalls return EINTR
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif
}

void installJsInterruptHandler(JSRuntime* rt) {
    if (!rt) return;
    JS_SetInterruptHandler(rt, jsInterruptHandler, nullptr);
}

} // namespace bro::util
