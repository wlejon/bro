// `performance`: the rAF clock behind `now()`, and the User Timing surface
// over it — mark / measure / getEntries* / clearMarks / clearMeasures — plus
// `timeOrigin`.
//
// `now()` is the host clock (hostClockMs), so performance.now() and rAF
// timestamps agree — the invariant three.js's Clock leans on. It advances only
// with frames, which is also what keeps it honest under bro.time pause and
// headless virtual time.
//
// The User Timing half used to come from brokit's timers polyfill, which bro
// no longer installs (host_timers.cpp owns the timers, on the engine clock);
// it is kept here on the SAME clock, because a `measure` between two marks
// taken across advanceTime() steps must answer the virtual span, not a wall
// one. Entries are PerformanceEntry-shaped plain objects: {name, entryType,
// startTime, duration} plus `detail` on a mark, which is what every profiler
// helper and the odd library assertion reads.

#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

struct Entry {
    std::string name;
    std::string entryType;  // "mark" | "measure"
    double startTime = 0.0;
    double duration = 0.0;
};

struct Timing {
    std::vector<Entry> entries;
    double timeOrigin = 0.0;  // wall ms at install, as the web's timeOrigin is
};

std::string stringArg(std::span<const Value> a, size_t i) {
    Value v = argAt(a, i);
    if (ev::isUndefined(v) || ev::isObject(v)) return std::string();
    return ev::toUtf8(v);
}

// The most recent mark with this name, or null.
const Entry* findMark(const Timing& t, const std::string& name) {
    for (size_t i = t.entries.size(); i-- > 0;) {
        if (t.entries[i].entryType == "mark" && t.entries[i].name == name) return &t.entries[i];
    }
    return nullptr;
}

Value entryValue(const Entry& e) {
    ObjectBuilder b;
    b.set("name", ev::fromUtf8(e.name));
    b.set("entryType", ev::fromUtf8(e.entryType));
    b.set("startTime", ev::fromDouble(e.startTime));
    b.set("duration", ev::fromDouble(e.duration));
    if (e.entryType == "mark") b.set("detail", ev::null());
    b.def("toJSON", 0, [](Value self, std::span<const Value>) -> Value {
        ObjectBuilder j;
        j.set("name", ev::getProperty(self, "name"));
        j.set("entryType", ev::getProperty(self, "entryType"));
        j.set("startTime", ev::getProperty(self, "startTime"));
        j.set("duration", ev::getProperty(self, "duration"));
        return j.get();
    });
    return b.get();
}

Value entryList(const std::vector<const Entry*>& list) {
    return hostArrayOf(list.size(), [&list](size_t i) { return entryValue(*list[i]); });
}

std::vector<const Entry*> select(const Timing& t, const std::string* name, const std::string* type) {
    std::vector<const Entry*> out;
    for (const Entry& e : t.entries) {
        if (name && e.name != *name) continue;
        if (type && e.entryType != *type) continue;
        out.push_back(&e);
    }
    return out;
}

void clearOfType(Timing& t, const std::string& type, const std::string* name) {
    auto it = t.entries.begin();
    while (it != t.entries.end()) {
        if (it->entryType == type && (!name || it->name == *name)) it = t.entries.erase(it);
        else ++it;
    }
}

}  // namespace

Value makePerformanceValue() {
    auto timing = std::make_shared<Timing>();
    timing->timeOrigin = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) / 1000.0;

    ObjectBuilder b;
    b.def("now", 0, [](Value, std::span<const Value>) {
        return ev::fromDouble(hostClockMs());
    });
    b.set("timeOrigin", ev::fromDouble(timing->timeOrigin));

    // mark(name, {startTime?, detail?}) -> the entry
    b.def("mark", 2, [timing](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("performance.mark: a name is required");
        Entry e;
        e.name = ev::isObject(a[0]) ? std::string("[object Object]") : ev::toUtf8(a[0]);
        e.entryType = "mark";
        e.startTime = hostClockMs();
        if (a.size() > 1 && ev::isObject(a[1])) {
            Value st = ev::getProperty(a[1], "startTime");
            if (ev::isNumber(st)) e.startTime = ev::toDouble(st);
        }
        timing->entries.push_back(e);
        return entryValue(e);
    });

    // measure(name, startMark?, endMark?) or measure(name, {start, end, duration})
    b.def("measure", 3, [timing](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("performance.measure: a name is required");
        Entry e;
        e.name = ev::isObject(a[0]) ? std::string("[object Object]") : ev::toUtf8(a[0]);
        e.entryType = "measure";
        double start = 0.0;
        double end = hostClockMs();
        auto resolve = [&](Value v, double& out) -> bool {
            if (ev::isNumber(v)) { out = ev::toDouble(v); return true; }
            if (ev::isString(v)) {
                const Entry* m = findMark(*timing, ev::toUtf8(v));
                if (!m) return false;
                out = m->startTime;
                return true;
            }
            return true;  // absent: keep the default
        };
        if (a.size() > 1 && ev::isObject(a[1])) {
            Value sV = ev::getProperty(a[1], "start");
            Value eV = ev::getProperty(a[1], "end");
            Value dV = ev::getProperty(a[1], "duration");
            if (!resolve(sV, start) || !resolve(eV, end)) {
                return ev::throwError("performance.measure: no mark with that name");
            }
            if (ev::isNumber(dV)) {
                if (ev::isUndefined(eV) || ev::isNull(eV)) end = start + ev::toDouble(dV);
                else if (ev::isUndefined(sV) || ev::isNull(sV)) start = end - ev::toDouble(dV);
            }
        } else {
            if (a.size() > 1 && !resolve(a[1], start)) {
                return ev::throwError("performance.measure: no mark named " + ev::toUtf8(a[1]));
            }
            if (a.size() > 2 && !resolve(a[2], end)) {
                return ev::throwError("performance.measure: no mark named " + ev::toUtf8(a[2]));
            }
        }
        e.startTime = start;
        e.duration = end - start;
        timing->entries.push_back(e);
        return entryValue(e);
    });

    b.def("getEntries", 0, [timing](Value, std::span<const Value>) -> Value {
        return entryList(select(*timing, nullptr, nullptr));
    });
    b.def("getEntriesByName", 2, [timing](Value, std::span<const Value> a) -> Value {
        std::string name = stringArg(a, 0);
        std::string type = stringArg(a, 1);
        return entryList(select(*timing, &name, type.empty() ? nullptr : &type));
    });
    b.def("getEntriesByType", 1, [timing](Value, std::span<const Value> a) -> Value {
        std::string type = stringArg(a, 0);
        return entryList(select(*timing, nullptr, &type));
    });
    b.def("clearMarks", 1, [timing](Value, std::span<const Value> a) -> Value {
        std::string name = stringArg(a, 0);
        clearOfType(*timing, "mark", a.empty() || ev::isUndefined(a[0]) ? nullptr : &name);
        return ev::undefined();
    });
    b.def("clearMeasures", 1, [timing](Value, std::span<const Value> a) -> Value {
        std::string name = stringArg(a, 0);
        clearOfType(*timing, "measure", a.empty() || ev::isUndefined(a[0]) ? nullptr : &name);
        return ev::undefined();
    });
    b.def("clearResourceTimings", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
    b.def("toJSON", 0, [timing](Value, std::span<const Value>) -> Value {
        ObjectBuilder j;
        j.set("timeOrigin", ev::fromDouble(timing->timeOrigin));
        return j.get();
    });
    return b.get();
}

}  // namespace bro::bronze_host
