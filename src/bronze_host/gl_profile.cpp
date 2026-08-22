// The BRO_GL_PROFILE seam. gl_profile.h holds the reasoning; this file is the
// mechanism: a slot table built at install time, a two-timestamp wrapper, and
// a sorted dump at exit.

// getenv, as bronze's runtime/profile.cpp does it: the targeted CRT
// deprecation opt-out rather than a blanket C4996 disable.
#define _CRT_SECURE_NO_WARNINGS

#include "bronze_host/gl_profile.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#endif

namespace bro::bronze_host {
namespace {

namespace ev = bronze::embed;

// --- the clock -------------------------------------------------------------

#ifdef _WIN32
inline uint64_t ticksNow() {
    LARGE_INTEGER t;
    ::QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}
inline double ticksPerSecond() {
    LARGE_INTEGER f;
    ::QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
}
#else
inline uint64_t ticksNow() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
inline double ticksPerSecond() { return 1e9; }
#endif

// --- the slot table --------------------------------------------------------

struct Slot {
    std::string name;
    uint64_t count = 0;
    uint64_t selfTicks = 0;
    uint64_t inclTicks = 0;
};

// std::map, not unordered_map: slots are handed out by pointer and must never
// move, and node-based containers are the ones that promise that. Touched only
// at install time, so the ordered lookup costs nothing measurable.
std::map<std::string, Slot>& slotTable() {
    static std::map<std::string, Slot> table;
    return table;
}

// Time charged to callees of the call currently on top of this thread's stack.
// A host call that re-enters JS (an event listener, a promise drain) would
// otherwise bill its callee's time to itself.
thread_local uint64_t t_childTicks = 0;

bool s_enabled = false;
bool s_trace = false;
bool s_initialised = false;
bool s_dumped = false;

void initOnce() {
    if (s_initialised) return;
    s_initialised = true;
    const char* env = std::getenv("BRO_GL_PROFILE");
    if (env && std::strcmp(env, "trace") == 0) {
        s_enabled = true;
        s_trace = true;
        slotTable();
        std::atexit(hostProfileDump);
        return;
    }
    if (env && std::strcmp(env, "1") == 0) {
        s_enabled = true;
        slotTable();  // construct before atexit registers, so it outlives the dump
        std::atexit(hostProfileDump);
    }
}

Slot* slotFor(const char* name) {
    auto& table = slotTable();
    auto it = table.find(name);
    if (it == table.end()) {
        it = table.emplace(name, Slot{}).first;
        it->second.name = name;
    }
    return &it->second;
}

}  // namespace

bool hostProfileEnabled() {
    initOnce();
    return s_enabled;
}

ev::NativeFn hostProfileWrap(const char* name, ev::NativeFn fn) {
    initOnce();
    if (!s_enabled || !name) return fn;

    Slot* slot = slotFor(name);
    return [slot, inner = std::move(fn)](bronze::Value thisValue,
                                        std::span<const bronze::Value> args) {
        if (s_trace) std::fprintf(stderr, "HOSTCALL %s argc=%zu\n", slot->name.c_str(), args.size());
        const uint64_t savedChild = t_childTicks;
        t_childTicks = 0;
        const uint64_t t0 = ticksNow();
        bronze::Value result = inner(thisValue, args);
        const uint64_t dt = ticksNow() - t0;
        const uint64_t child = t_childTicks;
        t_childTicks = savedChild + dt;
        slot->count++;
        slot->inclTicks += dt;
        slot->selfTicks += (dt > child) ? (dt - child) : 0;
        return result;
    };
}

void hostProfileDump() {
    if (!s_enabled || s_dumped) return;
    s_dumped = true;

    std::vector<const Slot*> slots;
    uint64_t totalCalls = 0;
    uint64_t totalSelf = 0;
    for (const auto& [key, slot] : slotTable()) {
        if (slot.count == 0) continue;
        slots.push_back(&slot);
        totalCalls += slot.count;
        totalSelf += slot.selfTicks;
    }
    if (slots.empty()) return;

    std::sort(slots.begin(), slots.end(), [](const Slot* a, const Slot* b) {
        if (a->selfTicks != b->selfTicks) return a->selfTicks > b->selfTicks;
        return a->name < b->name;
    });

    const double perSec = ticksPerSecond();
    const double msPerTick = 1000.0 / perSec;
    const double nsPerTick = 1e9 / perSec;

    std::fprintf(stderr, "\n=== Bro Host Native Call Profile (BRO_GL_PROFILE=1) ===\n");
    std::fprintf(stderr, "entry points called : %zu\n", slots.size());
    std::fprintf(stderr, "total calls         : %llu\n",
                 static_cast<unsigned long long>(totalCalls));
    std::fprintf(stderr, "total self time     : %.3f ms\n\n", totalSelf * msPerTick);
    std::fprintf(stderr, "%-34s %12s %11s %11s %11s %8s\n", "entry point", "calls", "self ms",
                 "incl ms", "ns/call", "% self");
    std::fprintf(stderr,
                 "------------------------------------------------------------"
                 "--------------------------------\n");
    for (const Slot* s : slots) {
        const double selfMs = s->selfTicks * msPerTick;
        const double inclMs = s->inclTicks * msPerTick;
        const double nsCall = s->count ? (s->selfTicks * nsPerTick / s->count) : 0.0;
        const double pct = totalSelf ? (100.0 * s->selfTicks / totalSelf) : 0.0;
        std::fprintf(stderr, "%-34s %12llu %11.3f %11.3f %11.1f %7.1f%%\n", s->name.c_str(),
                     static_cast<unsigned long long>(s->count), selfMs, inclMs, nsCall, pct);
    }
    std::fprintf(stderr,
                 "------------------------------------------------------------"
                 "--------------------------------\n\n");
    std::fflush(stderr);
}

}  // namespace bro::bronze_host
