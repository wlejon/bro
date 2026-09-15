// `navigator`: the base object dom_gamepad.cpp builds (userAgent, platform,
// language, getGamepads), plus the two members that are not about input —
// `clipboard` over the platform clipboard and `getBattery()` over SDL's
// power query. Registered as a host global AND set on globalThis, like the
// other roots, so a compiled read and a dynamic one find the same object.

#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "engine/engine.h"
#include "platform/clipboard.h"

#include <SDL3/SDL_power.h>

#include <limits>
#include <string>

namespace bro::bronze_host {

namespace {

Value makeClipboardValue() {
    ObjectBuilder clip;
    clip.def("__read", 0, [](Value, std::span<const Value>) {
        return ev::fromUtf8(bro::platform::getClipboardText());
    });
    clip.def("__write", 1, [](Value, std::span<const Value> a) {
        Value textV = argAt(a, 0);
        std::string text = (!ev::isObject(textV) && !ev::isUndefined(textV)) ? ev::toUtf8(textV) : "";
        bool ok = bro::platform::setClipboardText(text);
        return ev::fromBool(ok);
    });
    clip.def("readText", 0, [](Value, std::span<const Value>) {
        Value p = ev::createPromise();
        ev::resolvePromise(p, ev::fromUtf8(bro::platform::getClipboardText()));
        return p;
    });
    clip.def("writeText", 1, [](Value, std::span<const Value> a) {
        Value textV = argAt(a, 0);
        std::string text = (!ev::isObject(textV) && !ev::isUndefined(textV)) ? ev::toUtf8(textV) : "";
        bool ok = bro::platform::setClipboardText(text);
        Value p = ev::createPromise();
        if (ok) {
            ev::resolvePromise(p, ev::undefined());
        } else {
            ev::Persistent promise(p);
            Value msg = ev::fromUtf8("clipboard write failed");
            ev::CallResult err = ev::construct(ev::globalValue("Error").value,
                                               std::span<const Value>(&msg, 1));
            ev::rejectPromise(promise.get(), err.value);
            return promise.get();
        }
        return p;
    });
    return clip.get();
}

// navigator.getBattery() — a Promise of a BatteryManager-shaped SNAPSHOT over
// SDL_GetPowerInfo. Snapshot-on-call: call again for fresh values; there are
// no change events, so the listener methods are present but inert and the
// `on*change` slots are null. A desktop without a battery reports the web's
// convention — charging, full, forever — and headless always reports that
// shape, so a test can assert exact values.
Value batterySnapshot() {
    const double inf = std::numeric_limits<double>::infinity();
    bool charging = true;
    double chargingTime = 0.0;     // seconds; 0 = full or no battery
    double dischargingTime = inf;  // seconds until empty
    double level = 1.0;            // 0.0 .. 1.0

    auto* eng = hostEngine();
    const bool headless = !eng || eng->displayMode() == engine::DisplayMode::Headless;
    if (!headless) {
        int secs = 0, pct = 0;
        switch (SDL_GetPowerInfo(&secs, &pct)) {
            case SDL_POWERSTATE_ON_BATTERY:
                charging = false;
                chargingTime = inf;
                dischargingTime = secs >= 0 ? static_cast<double>(secs) : inf;
                if (pct >= 0) level = pct / 100.0;
                break;
            case SDL_POWERSTATE_CHARGING:
                // SDL has no time-to-full estimate — unknown is Infinity per spec.
                chargingTime = inf;
                if (pct >= 0) level = pct / 100.0;
                break;
            case SDL_POWERSTATE_CHARGED:
                if (pct >= 0) level = pct / 100.0;
                break;
            case SDL_POWERSTATE_NO_BATTERY:
            case SDL_POWERSTATE_UNKNOWN:
            case SDL_POWERSTATE_ERROR:
            default:
                break;  // keep the desktop no-battery shape
        }
    }

    ObjectBuilder b;
    b.set("charging", ev::fromBool(charging));
    b.set("chargingTime", ev::fromDouble(chargingTime));
    b.set("dischargingTime", ev::fromDouble(dischargingTime));
    b.set("level", ev::fromDouble(level));
    // EventTarget veneer so spec-shaped code does not throw; never fires.
    b.def("addEventListener", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
    b.def("removeEventListener", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
    for (const char* slot : {"onchargingchange", "onchargingtimechange",
                             "ondischargingtimechange", "onlevelchange"}) {
        b.set(slot, ev::null());
    }
    return b.get();
}

}  // namespace

void installNavigatorGlobal() {
    ObjectBuilder nav(makeNavigatorValue());
    {
        Value clip = makeClipboardValue();
        nav.set("clipboard", clip);
    }
    nav.def("getBattery", 0, [](Value, std::span<const Value>) {
        ev::Persistent snapshot(batterySnapshot());
        ev::Persistent p(ev::createPromise());
        ev::resolvePromise(p.get(), snapshot.get());
        return p.get();
    });

    ev::registerGlobal("navigator", nav.get());
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) ev::setProperty(gt.value, "navigator", nav.get());
}

}  // namespace bro::bronze_host
