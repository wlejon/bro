#include "bronze_host/host_headless.h"
#include "bronze_host/host_headless_internal.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_brokit.h"
#include "bronze_host/host_gc.h"
#include "bronze_host/host_js_modules.h"
#include "bronze_host/host_storage.h"
#include "bronze_host/host_natives.h"  // pollNet
#include "bronze_host/host_builder.h"

#include "engine/engine.h"
#include "bronze_host/host_bro_namespaces.h"
#include "platform/desktop_notifications.h"
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
static std::atomic<bool> s_testSkipped{false};
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

bool wasTestSkipped() {
    return s_testSkipped;
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
            drainMicrotasksAndLocalFetches();
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
            drainMicrotasksAndLocalFetches();
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
            drainMicrotasksAndLocalFetches();
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

    // 4b. runFrames(n [, { stepMs }]): n frames of the windowed frame loop
    // itself — layout and raster threads, presenter to an offscreen target,
    // idle holds — in real time (paced at 60 Hz), or with stepMs > 0 on a
    // clock stepped that much per frame. Returns { frames, presented, held,
    // wallMs }. docs/headless.md, "The windowed pipeline".
    regBoth("runFrames", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            const int n = a.empty() ? 1 : satCast<int>(ev::toDouble(a[0]));
            double stepMs = 0.0;
            if (a.size() > 1 && ev::isObject(a[1])) {
                const Value s = ev::getProperty(a[1], "stepMs");
                if (!ev::isUndefined(s)) stepMs = ev::toDouble(s);
            }
            engine::Engine::PipelineRun run;
            {
                // The frames collect as a window's do (idle and pressure
                // GC), not as frames under a host eval, which never do.
                HostEvalSuspend frames;
                run = engine.runPipelineFrames(n, stepMs);
            }
            drainMicrotasksAndLocalFetches();
            ev::Persistent o(ev::createObject());
            o.set(ev::setProperty(o.get(), "frames", ev::fromDouble(run.frames)));
            o.set(ev::setProperty(o.get(), "presented", ev::fromDouble(run.presented)));
            o.set(ev::setProperty(o.get(), "held", ev::fromDouble(run.held)));
            o.set(ev::setProperty(o.get(), "wallMs", ev::fromDouble(run.wallMs)));
            return o.get();
        }, 2, "runFrames"));

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

    // 5b. skipTest(reason): this environment cannot test the script's subject
    // (a model's weights are absent, a feature is compiled out). The run exits
    // 77 — reported SKIP, never PASS — unless something also failed. The
    // script keeps running, so it should do nothing more after the call.
    regBoth("skipTest", ev::makeFunction(
        [](Value, std::span<const Value> a) -> Value {
            std::string reason = a.empty() ? "no reason given" : ev::toUtf8(a[0]);
            LOG_INFO("SKIP: %s", reason.c_str());
            s_testSkipped = true;
            return ev::undefined();
        }, 1, "skipTest"));

    // 5c. missingGpuContext(kind): getContext(kind) returned null. Only a run
    // with no GPU device (--no-gpu, or a Vulkan device that would not come up
    // under BRO_TEST_ALLOW_RASTER) or a build without the feature is allowed
    // that: it SKIPs. On a GPU run a null 'webgl2' / 'scene' context is a bug
    // and FAILS the run, instead of passing it untested.
    regBoth("missingGpuContext", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            std::string kind = a.empty() ? "gpu" : ev::toUtf8(a[0]);
            const bool compiledOut = kind == "scene" && !BRO_WITH_3D;
            if (engine.vulkanContext() == nullptr || compiledOut) {
                LOG_INFO("SKIP: no '%s' context (%s)", kind.c_str(),
                         compiledOut ? "3D compiled out" : "no GPU device");
                s_testSkipped = true;
            } else {
                LOG_ERROR("ASSERTION FAILED: getContext('%s') returned null on a GPU run",
                          kind.c_str());
                setTestFailure(true);
                engine.setTestFailure(true);
            }
            return ev::undefined();
        }, 1, "missingGpuContext"));

    // 6. resize(int w, int h)
    regBoth("resize", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 2) return ev::throwTypeError("resize(w, h) requires width and height");
            int w = satCast<int>(ev::toDouble(a[0]));
            int h = satCast<int>(ev::toDouble(a[1]));
            engine.handleResize(w, h);
            engine.flush();
            return ev::undefined();
        }, 2, "resize"));

    // 6b. setDeviceScaleFactor(scale): render as a display with `scale` device
    // px per CSS px — devicePixelRatio, @media (resolution), layer surfaces and
    // screenshots follow; layout and event coordinates stay in CSS px.
    regBoth("setDeviceScaleFactor", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.empty()) return ev::throwTypeError("setDeviceScaleFactor(scale) requires a scale");
            double s = ev::toDouble(a[0]);
            if (!(s > 0.0)) return ev::throwRangeError("setDeviceScaleFactor: scale must be > 0");
            engine.setDeviceScaleFactor(static_cast<float>(s));
            engine.flush();
            return ev::undefined();
        }, 1, "setDeviceScaleFactor"));

    // 6c. openedApps(): what bro.app.open was asked to start (headless starts
    // nothing): [{ id, dir, from, command, args, cwd, spawned }], oldest
    // first. openedApps({ clear: true }) also forgets them.
    regBoth("openedApps", ev::makeFunction(
        [](Value, std::span<const Value> a) -> Value {
            const bool clear = !a.empty() && ev::isObject(a[0]) && ev::toBool(ev::getProperty(a[0], "clear"));
            Value v = appOpenRecordsValue();
            if (clear) {
                ev::Persistent keep(v);
                clearAppOpenRecords();
                return keep.get();
            }
            return v;
        }, 1, "openedApps"));

    // 6d. notifications(): the desktop notifications the page posted
    // (Notification, bro.window.notify), which headless records and does not
    // show: [{ id, title, body, icon, silent, timeout, replacesId, appId,
    // appName, via }].
    // notifications({ clear: true }) also forgets them.
    regBoth("notifications", ev::makeFunction(
        [](Value, std::span<const Value> a) -> Value {
            const bool clear = !a.empty() && ev::isObject(a[0]) && ev::toBool(ev::getProperty(a[0], "clear"));
            auto list = platform::desktop::getRecordedNotifications();
            if (clear) platform::desktop::clearRecordedNotifications();
            return hostArrayOf(list.size(), [&list](size_t i) -> Value {
                const auto& n = list[i];
                ObjectBuilder o;
                o.set("id", ev::fromDouble(double(n.id)));
                o.set("title", ev::fromUtf8(n.title));
                o.set("body", ev::fromUtf8(n.body));
                o.set("icon", ev::fromUtf8(n.options.icon));
                o.set("silent", ev::fromBool(n.options.silent));
                o.set("timeout", ev::fromDouble(double(n.options.timeoutMs)));
                o.set("replacesId", ev::fromDouble(double(n.options.replacesId)));
                o.set("appId", ev::fromUtf8(n.options.appId));
                o.set("appName", ev::fromUtf8(n.options.appName));
                o.set("via", ev::fromUtf8(n.via));
                return o.get();
            });
        }, 1, "notifications"));

    // 6e. lastFileDialogFilter(): the filter the last open/save file dialog
    // was asked for ("Accepted files|wav;mp3;..." for <input accept>), "" for none.
    regBoth("lastFileDialogFilter", ev::makeFunction(
        [](Value, std::span<const Value>) -> Value {
            return ev::fromUtf8(platform::Dialogs::lastFileFilter());
        }, 0, "lastFileDialogFilter"));

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
                    const uint32_t n = satCast<uint32_t>(ev::toDouble(ev::getProperty(v, "length")));
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
