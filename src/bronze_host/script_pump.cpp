// The script watchdog (script_pump.h).

#include "bronze_host/script_pump.h"

#include "util/page_pump.h"

#include "embed/embed.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace bro::bronze_host {

namespace {

namespace embed = bronze::embed;

std::mutex g_mutex;
std::condition_variable g_cv;
std::thread g_thread;
bool g_stop = false;

void onInterrupt(void*) { util::pumpPageThread(util::PumpReason::Interrupt); }

void watch(embed::HostInterrupt* target) {
    std::unique_lock<std::mutex> lock(g_mutex);
    while (!g_stop) {
        if (!util::longestPumpGap().running) break;  // the first frame is up
        if (util::msSinceLastPagePump() >= util::kPumpIntervalMs) embed::requestHostInterrupt(target);
        g_cv.wait_for(lock, std::chrono::milliseconds(util::kPumpIntervalMs / 2));
    }
}

}  // namespace

void startScriptPumpWatchdog() {
    if (g_thread.joinable() || !util::longestPumpGap().running) return;
    embed::HostInterrupt* target = embed::registerHostInterrupt(&onInterrupt, nullptr);
    g_stop = false;
    g_thread = std::thread(watch, target);
}

void stopScriptPumpWatchdog() {
    if (!g_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
    }
    g_cv.notify_all();
    g_thread.join();
    embed::unregisterHostInterrupt();
}

}  // namespace bro::bronze_host
