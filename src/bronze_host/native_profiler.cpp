// `__bro_native.profiler` — the C entry points behind bro.profiler
// (docs/profiler-api.js): bronze's sampling profiler (bronze/src/embed/
// embed_profiler.h) started and stopped from script, its result handed back as
// JSON that js/profiler.js parses. Also the thread registrations that decide
// what a profile can see: the main thread as the realm is built, each Worker's
// thread for its lifetime (ProfilerThreadScope).

#include "bronze_host/host_class.h"
#include "bronze_host/host_natives.h"
#include "embed/embed_profiler.h"
#include "natives/profiler/native_profiler_decl.h"

#include <cstdio>
#include <string>

namespace {

namespace rt = bronze::runtime;

void appendJsonString(std::string& out, const std::string& s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(c));
                    out += b;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

std::string resultJson(const rt::ProfilerResult& r, bool callers, bool report) {
    std::string j;
    j.reserve(256 + r.functions.size() * 128 + r.edges.size() * 32 + r.text.size());
    char num[64];
    auto u64 = [&](uint64_t v) {
        std::snprintf(num, sizeof num, "%llu", static_cast<unsigned long long>(v));
        j += num;
    };
    j += "{\"hz\":";
    u64(r.hz);
    std::snprintf(num, sizeof num, ",\"durationMs\":%.3f", r.durationMs);
    j += num;
    j += ",\"samples\":";
    u64(r.samples);
    j += ",\"truncated\":";
    j += r.truncated ? "true" : "false";
    j += ",\"threads\":[";
    for (size_t i = 0; i < r.threads.size(); ++i) {
        const rt::ProfilerThread& t = r.threads[i];
        if (i) j += ',';
        j += "{\"id\":";
        u64(t.id);
        j += ",\"kind\":";
        appendJsonString(j, t.kind);
        j += ",\"name\":";
        appendJsonString(j, t.name);
        j += ",\"samples\":";
        u64(t.samples);
        j += '}';
    }
    j += "],\"functions\":[";
    for (size_t i = 0; i < r.functions.size(); ++i) {
        const rt::ProfilerFunction& f = r.functions[i];
        if (i) j += ',';
        j += "{\"name\":";
        appendJsonString(j, f.name);
        j += ",\"tier\":";
        appendJsonString(j, f.tier);
        j += ",\"module\":";
        appendJsonString(j, f.module);
        if (!f.file.empty()) {
            j += ",\"file\":";
            appendJsonString(j, f.file);
            j += ",\"line\":";
            u64(f.line);
        }
        if (!f.tier1Rejected.empty()) {
            j += ",\"tier1Rejected\":";
            appendJsonString(j, f.tier1Rejected);
        }
        j += ",\"self\":";
        u64(f.self);
        j += ",\"total\":";
        u64(f.total);
        j += '}';
    }
    j += ']';
    if (callers) {
        j += ",\"callers\":[";
        for (size_t i = 0; i < r.edges.size(); ++i) {
            const rt::ProfilerEdge& e = r.edges[i];
            if (i) j += ',';
            j += "{\"caller\":";
            u64(e.caller);
            j += ",\"callee\":";
            u64(e.callee);
            j += ",\"count\":";
            u64(e.count);
            j += '}';
        }
        j += ']';
    }
    if (report) {
        j += ",\"report\":";
        appendJsonString(j, r.text);
    }
    j += '}';
    return j;
}

}  // namespace

extern "C" {

const char* bro_profiler_start(double hz, const char* threads) {
    rt::ProfilerOptions o;
    o.hz = hz > 0 ? static_cast<uint32_t>(hz) : 1000;
    const std::string which = threads ? threads : "main";
    if (which == "main") {
        o.threads = rt::kProfilerThreadMain;
    } else if (which == "workers") {
        o.threads = rt::kProfilerThreadWorker;
    } else if (which == "js") {
        o.threads = rt::kProfilerThreadMain | rt::kProfilerThreadWorker;
    } else if (which == "all") {
        o.threads = rt::kProfilerThreadMain | rt::kProfilerThreadWorker | rt::kProfilerThreadOther;
        o.processThreads = true;
    } else {
        return bro::bronze_host::natives::strResult(
            "bro.profiler.start: threads must be 'main', 'workers', 'js' or 'all', not '" + which + "'");
    }
    std::string err;
    if (!bronze::embed::profilerStart(o, &err)) return bro::bronze_host::natives::strResult("bro.profiler.start: " + err);
    return "";
}

bool bro_profiler_running(void) { return bronze::embed::profilerRunning(); }

const char* bro_profiler_stop(bool callers, bool report, int32_t top) {
    rt::ProfilerStopOptions o;
    o.callers = callers;
    o.text = report;
    o.top = top > 0 ? static_cast<uint32_t>(top) : 40;
    rt::ProfilerResult r;
    std::string err;
    if (!bronze::embed::profilerStop(o, r, &err)) return "";
    return bro::bronze_host::natives::strResult(resultJson(r, callers, report));
}

}  // extern "C"

namespace bro::bronze_host {

bool registerNatives_profiler(std::string* error);

bool registerProfilerNatives(std::string* error) { return registerNatives_profiler(error); }

void registerProfilerMainThread() { bronze::embed::profilerRegisterThread(rt::kProfilerThreadMain, "main"); }

ProfilerThreadScope::ProfilerThreadScope(const std::string& name) {
    bronze::embed::profilerRegisterThread(rt::kProfilerThreadWorker, name);
}

ProfilerThreadScope::~ProfilerThreadScope() { bronze::embed::profilerUnregisterThread(); }

}  // namespace bro::bronze_host
