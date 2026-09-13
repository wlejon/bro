#include "bronze_host/host_headless.h"
#include "bronze_host/host_headless_internal.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"

#include "engine/engine.h"
#include "platform/dialogs.h"
#include "util/log.h"

#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include <span>

namespace bro::bronze_host {

namespace {

static bool s_hasTestFailure = false;
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
        ev::registerGlobal("scriptArgs", makeScriptArgsValue());
    }
}

void installHeadlessGlobals(engine::Engine& engine) {
    installHeadlessInput(engine);
    installHeadlessFrame(engine);
    installHeadlessTestHooks(engine);

    // 1. advanceTime(double ms)
    ev::registerGlobal("advanceTime", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            double ms = a.empty() ? 0.0 : ev::toDouble(a[0]);
            engine.advanceTime(ms);
            deliverHostObservers();
            if (ev::microtasksPending()) ev::drainMicrotasks();
            return ev::undefined();
        }, 1, "advanceTime"));

    // 2. flush()
    ev::registerGlobal("flush", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            engine.flush();
            deliverHostObservers();
            if (ev::microtasksPending()) ev::drainMicrotasks();
            return ev::undefined();
        }, 0, "flush"));

    // 3. sleep(double ms)
    ev::registerGlobal("sleep", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            double ms = a.empty() ? 0.0 : ev::toDouble(a[0]);
            engine.advanceTime(ms);
            if (ev::microtasksPending()) ev::drainMicrotasks();
            return ev::undefined();
        }, 1, "sleep"));

    // 4. wallSleep(double ms)
    ev::registerGlobal("wallSleep", ev::makeFunction(
        [](Value, std::span<const Value> a) -> Value {
            double ms = a.empty() ? 0.0 : ev::toDouble(a[0]);
            if (ms > 0.0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>(ms)));
            }
            return ev::undefined();
        }, 1, "wallSleep"));

    // 5. assert(bool condition [, const std::string& message])
    ev::registerGlobal("assert", ev::makeFunction(
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
    ev::registerGlobal("resize", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 2) return ev::throwTypeError("resize(w, h) requires width and height");
            int w = static_cast<int>(ev::toDouble(a[0]));
            int h = static_cast<int>(ev::toDouble(a[1]));
            engine.handleResize(w, h);
            engine.flush();
            return ev::undefined();
        }, 2, "resize"));

    // 7. setDialogAnswer(accept)
    ev::registerGlobal("setDialogAnswer", ev::makeFunction(
        [](Value, std::span<const Value> a) {
            platform::Dialogs::setAutoDialogAnswer(a.empty() ? true : ev::toBool(a[0]));
            return ev::undefined();
        }, 1, "setDialogAnswer"));

    // 8. setPickedFiles(paths)
    ev::registerGlobal("setPickedFiles", ev::makeFunction(
        [](Value, std::span<const Value> a) {
            std::vector<std::string> paths;
            if (!a.empty()) {
                Value v = a[0];
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
    ev::registerGlobal("scriptArgs", makeScriptArgsValue());
}

} // namespace bro::bronze_host
