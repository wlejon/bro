// Keeping the page thread answering while it is busy (page_pump.h).

#include "util/page_pump.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace bro::util {

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<std::thread::id> g_pageThread{};
std::atomic<PagePump> g_pump{nullptr};
thread_local bool t_inPump = false;

double nowMs() {
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

// The last pump, read by the script watchdog's thread.
std::atomic<double> g_lastPumpMs{0.0};

std::mutex g_watchMutex;
PumpGap g_gap;
double g_watchStartMs = 0.0;

}  // namespace

void setPageThread() { g_pageThread.store(std::this_thread::get_id(), std::memory_order_relaxed); }

bool onPageThread() { return g_pageThread.load(std::memory_order_relaxed) == std::this_thread::get_id(); }

void setPagePump(PagePump pump) { g_pump.store(pump, std::memory_order_release); }

bool inPagePump() { return t_inPump; }

bool pumpsWhileWaiting() { return g_pump.load(std::memory_order_acquire) != nullptr && onPageThread(); }

void pumpPageThread(PumpReason reason) {
    if (t_inPump || !onPageThread()) return;
    PagePump pump = g_pump.load(std::memory_order_acquire);
    if (!pump) return;
    t_inPump = true;
    pump(reason);
    t_inPump = false;
    notePagePump();
}

namespace {

void noteGap(double last, double now) {
    std::lock_guard<std::mutex> lock(g_watchMutex);
    if (!g_gap.running) return;
    const double from = last > g_watchStartMs ? last : g_watchStartMs;
    if (now - from > g_gap.longestMs) {
        g_gap.longestMs = now - from;
        g_gap.endedAtMs = now - g_watchStartMs;
    }
}

}  // namespace

void notePagePump() {
    if (!onPageThread()) return;
    const double now = nowMs();
    noteGap(g_lastPumpMs.exchange(now, std::memory_order_relaxed), now);
}

double msSinceLastPagePump() { return nowMs() - g_lastPumpMs.load(std::memory_order_relaxed); }

void startPumpWatch() {
    std::lock_guard<std::mutex> lock(g_watchMutex);
    if (g_gap.running) return;
    g_gap = PumpGap{};
    g_gap.running = true;
    g_watchStartMs = nowMs();
}

void stopPumpWatch() {
    // The stretch up to this point counts (from whichever thread presented).
    noteGap(g_lastPumpMs.load(std::memory_order_relaxed), nowMs());
    std::lock_guard<std::mutex> lock(g_watchMutex);
    g_gap.running = false;
}

PumpGap longestPumpGap() {
    std::lock_guard<std::mutex> lock(g_watchMutex);
    return g_gap;
}

}  // namespace bro::util
