#include "bronze_host/host_headless.h"
#include "bronze_host/host_headless_internal.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_storage.h"
#include "bronze_host/host_natives.h"  // pollNet
#include "bronze_host/gl_internal.h"

#include "engine/engine.h"
#include "platform/dialogs.h"
#include "util/log.h"

#if BRO_WITH_AUDIO
#include <broaudio/api.h>
#endif

#include <atomic>
#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include <span>

namespace bro::bronze_host {

namespace {

// Atomic: a worker thread's uncancelled unhandled rejection fails the run too
// (host_worker.cpp), and it reports from its own thread.
static std::atomic<bool> s_hasTestFailure{false};
static std::vector<std::string> s_scriptArgs;

static Value makeScriptArgsValue() {
    ev::CallResult parsed = ev::parseJson("[]");
    ev::Persistent arr{parsed.value};
    for (uint32_t i = 0; i < s_scriptArgs.size(); ++i) {
        ev::Persistent s{ev::fromUtf8(s_scriptArgs[i])};
        arr.set(ev::setElement(arr.get(), i, s.get()));
    }
    return arr.get();
}

} // namespace

bool hasTestFailure() {
    return s_hasTestFailure;
}

void clearTestFailure() {
    s_hasTestFailure = false;
}

void setTestFailure(bool failed) {
    s_hasTestFailure = failed;
}

void setScriptArgs(const std::vector<std::string>& args) {
    s_scriptArgs = args;
    if (isWebHostGlobalsInstalled()) {
        const Rooted val(makeScriptArgsValue());  // registerGlobal allocates
        ev::registerGlobal("scriptArgs", val);
        ev::GlobalValue gt = ev::globalValue("globalThis");
        if (gt.found && ev::isObject(gt.value)) {
            ev::setProperty(gt.value, "scriptArgs", val);
        }
    }
}

void installHeadlessGlobals(engine::Engine& engine) {
    installHeadlessInput(engine);
    installHeadlessFrame(engine);
    installHeadlessTestHooks(engine);

    ev::GlobalValue gt = ev::globalValue("globalThis");
    // Rooted: every registration below allocates (the function, and
    // registerGlobal's define on the live realms).
    const Rooted gObj(gt.found && ev::isObject(gt.value) ? gt.value : Value::fromUndefined());

    auto regBoth = [&](const char* name, Value valIn) {
        const Rooted val(valIn);
        ev::registerGlobal(name, val);
        if (ev::isObject(gObj)) {
            ev::setProperty(gObj, name, val);
        }
    };

    // 1. advanceTime(double ms)
    regBoth("advanceTime", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            double ms = a.empty() ? 0.0 : ev::toDouble(a[0]);
            engine.advanceTime(ms);
#if BRO_WITH_AUDIO
            broaudio::api::drainMicChunks();
#endif
            pumpBrokitTicks();
            if (ev::microtasksPending()) ev::drainMicrotasks();
            return ev::undefined();
        }, 1, "advanceTime"));

    // 2. flush(): lay out and paint without advancing the clock. Engine::flush
    // does not run the frame callbacks (hostFrame), so the observer pass that
    // the frame seam would have run after layout — ResizeObserver over the
    // boxes flush just measured — is delivered here, as is the net poll; a
    // test that resizes, flushes and expects the observer to have seen it
    // relies on exactly that (tests/layout/test_layout_flush.js).
    regBoth("flush", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            flushHostStorage();
            engine.flush();
            pumpBrokitTicks();
            pollNet();
            fireHostObserverFrame();
            if (ev::microtasksPending()) ev::drainMicrotasks();
            return ev::undefined();
        }, 0, "flush"));

    // 3. sleep(double ms)
    regBoth("sleep", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            double ms = a.empty() ? 0.0 : ev::toDouble(a[0]);
            engine.advanceTime(ms);
#if BRO_WITH_AUDIO
            broaudio::api::drainMicChunks();
#endif
            pumpBrokitTicks();
            if (ev::microtasksPending()) ev::drainMicrotasks();
            return ev::undefined();
        }, 1, "sleep"));

    // 4. wallSleep(double ms)
    regBoth("wallSleep", ev::makeFunction(
        [](Value, std::span<const Value> a) -> Value {
            double ms = a.empty() ? 0.0 : ev::toDouble(a[0]);
            if (ms > 0.0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>(ms)));
            }
            return ev::undefined();
        }, 1, "wallSleep"));

    // 5. assert(bool condition [, const std::string& message])
    regBoth("assert", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            bool cond = !a.empty() && ev::toBool(a[0]);
            std::string msg = a.size() > 1 ? ev::toUtf8(a[1]) : "assertion failed";
            if (!cond) {
                LOG_ERROR("ASSERTION FAILED: %s", msg.c_str());
                setTestFailure(true);
                engine.setTestFailure(true);
                return ev::fromBool(false);
            }
            return ev::fromBool(true);
        }, 2, "assert"));

    // 6. resize(int w, int h)
    regBoth("resize", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 2) return ev::throwTypeError("resize(w, h) requires width and height");
            int w = static_cast<int>(ev::toDouble(a[0]));
            int h = static_cast<int>(ev::toDouble(a[1]));
            engine.handleResize(w, h);
            engine.flush();
            return ev::undefined();
        }, 2, "resize"));

    // 7. setDialogAnswer(accept)
    regBoth("setDialogAnswer", ev::makeFunction(
        [](Value, std::span<const Value> a) {
            platform::Dialogs::setAutoDialogAnswer(a.empty() ? true : ev::toBool(a[0]));
            return ev::undefined();
        }, 1, "setDialogAnswer"));

    // 8. setPickedFiles(paths)
    regBoth("setPickedFiles", ev::makeFunction(
        [](Value, std::span<const Value> a) {
            std::vector<std::string> paths;
            if (!a.empty()) {
                const Value& v = a[0];  // the rooted slot, current across the reads
                if (ev::isString(v)) {
                    paths.push_back(ev::toUtf8(v));
                } else if (ev::isObject(v)) {
                    const uint32_t n = static_cast<uint32_t>(ev::toDouble(ev::getProperty(v, "length")));
                    for (uint32_t i = 0; i < n; ++i) {
                        paths.push_back(ev::toUtf8(ev::getElement(v, i)));
                    }
                }
            }
            platform::Dialogs::setPickedFiles(std::move(paths));
            return ev::undefined();
        }, 1, "setPickedFiles"));

    // 9. scriptArgs
    regBoth("scriptArgs", makeScriptArgsValue());
}

} // namespace bro::bronze_host
