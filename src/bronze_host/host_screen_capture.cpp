// The desktop half of bro.window.captureScreen (docs/window-api.js,
// docs/screen-capture.md): every app's windows, from the OS
// (platform/screen_grab.h) or, under DRM, bro's own scanout
// (Engine::grabDesktop). Behind the `screen` capability.
//
//   bro.window.captureDisplays()        -> [{ index, name, x, y, width, height, scale, primary }]
//   bro.window._captureDesktop(options) -> Promise<{ x, y, width, height, display, backend, pixels?, path? }>
//
// js/bro_core.js takes _captureDesktop off bro.window into captureScreen's
// closure: captureScreen(options) is its public spelling.

#include "bronze_host/host_screen_capture.h"
#include "bronze_host/host_access.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"
#include "engine/app_permissions.h"
#include "engine/capture_path.h"
#include "engine/engine.h"
#include "util/log.h"

#include "broimage/encode.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

namespace scr = platform::screen;

Value errorWithCode(const std::string& message, const char* code) {
    ev::Persistent text(ev::fromUtf8(message));
    ev::Persistent err(text.get());
    auto ctor = ev::globalValue("Error");
    if (ctor.found && ev::isFunction(ctor.value)) {
        ev::Persistent c(ctor.value);
        const Value arg = text.get();
        auto r = ev::construct(c.get(), std::span<const Value>(&arg, 1));
        if (!r.thrown) err.set(r.value);
    }
    if (ev::isObject(err.get())) {
        ev::Persistent codeV(ev::fromUtf8(code));
        ev::setProperty(err.get(), "code", codeV.get());
    }
    return err.get();
}

Value rejected(const std::string& message, const char* code) {
    ev::Persistent promise(ev::createPromise());
    ev::Persistent err(errorWithCode("bro.window.captureScreen: " + message, code));
    ev::rejectPromise(promise.get(), err.get());
    return promise.get();
}

const char* codeFor(scr::Status s) {
    switch (s) {
        case scr::Status::Unsupported: return "ENOTSUP";
        case scr::Status::Denied: return "EPERM";
        default: return "EIO";
    }
}

Value displayValue(const scr::Display& d) {
    ObjectBuilder b;
    b.set("index", ev::fromDouble(d.index));
    b.set("name", ev::fromUtf8(d.name));
    b.set("x", ev::fromDouble(d.x));
    b.set("y", ev::fromDouble(d.y));
    b.set("width", ev::fromDouble(d.width));
    b.set("height", ev::fromDouble(d.height));
    b.set("scale", ev::fromDouble(d.scale));
    b.set("primary", ev::fromBool(d.primary));
    return b.get();
}

double numberProp(Value o, const char* key, double fallback) {
    Value v = ev::getProperty(o, key);
    return ev::isNumber(v) ? ev::toDouble(v) : fallback;
}

}  // namespace

void installBroWindowScreenCapture(Value broWinIn) {
    ObjectBuilder win(broWinIn);

    win.def("captureDisplays", 0, [](Value, std::span<const Value>) -> Value {
        auto* eng = hostEngine();
        std::vector<scr::Display> list;
        if (eng) list = eng->desktopDisplays();
        return hostArrayOf(list.size(), [&list](size_t i) { return displayValue(list[i]); });
    });

    win.def("_captureDesktop", 1, [](Value, std::span<const Value> a) -> Value {
        auto* eng = hostEngine();
        if (!eng) return rejected("no engine", "ENOTSUP");
        if (std::string refused = engine::appPermissions().use("screen", "capture the screen"); !refused.empty())
            return rejected(refused, "EACCES");

        std::string displayArg = "all";
        int displayIndex = -1;
        bool hasRect = false;
        double rx = 0, ry = 0, rw = 0, rh = 0;
        std::string path;
        bool wantPixels = true;
        bool cursor = false;
        if (hasArg(a, 0) && ev::isObject(a[0])) {
            ev::Persistent o(a[0]);
            Value d = ev::getProperty(o.get(), "display");
            if (ev::isNumber(d)) {
                displayIndex = static_cast<int>(ev::toDouble(d));
                displayArg = "index";
            } else if (ev::isString(d)) {
                displayArg = ev::toUtf8(d);
                if (displayArg != "all" && displayArg != "primary")
                    return rejected("display must be an index from captureDisplays(), 'primary' or 'all', got '" +
                                        displayArg + "'",
                                    "EINVAL");
            }
            Value r = ev::getProperty(o.get(), "rect");
            if (ev::isObject(r)) {
                ev::Persistent rect(r);
                hasRect = true;
                rx = numberProp(rect.get(), "x", 0);
                ry = numberProp(rect.get(), "y", 0);
                rw = numberProp(rect.get(), "width", 0);
                rh = numberProp(rect.get(), "height", 0);
                if (!(rw > 0) || !(rh > 0)) return rejected("rect needs a positive width and height", "EINVAL");
            }
            Value p = ev::getProperty(o.get(), "path");
            if (ev::isString(p)) path = ev::toUtf8(p);
            Value px = ev::getProperty(o.get(), "pixels");
            if (!ev::isUndefined(px) && !ev::isNull(px)) wantPixels = ev::toBool(px);
            cursor = ev::toBool(ev::getProperty(o.get(), "cursor"));
        }
        if (!path.empty()) {
            std::string asked;
            if (std::string refused = pathRefusal(path, true, &asked); !refused.empty()) {
                ev::Persistent promise(ev::createPromise());
                ev::Persistent err(errorWithCode(refused, "EACCES"));
                ev::Persistent pathV(ev::fromUtf8(asked.empty() ? path : asked));
                ev::setProperty(err.get(), "path", pathV.get());
                ev::rejectPromise(promise.get(), err.get());
                return promise.get();
            }
        }

        // The area: a display's (rect relative to it), else the desktop's
        // (rect relative to its top-left corner).
        std::string why;
        std::vector<scr::Display> list = eng->desktopDisplays(&why);
        if (list.empty()) return rejected(why.empty() ? "no display" : why, "ENOTSUP");
        int ax = list[0].x, ay = list[0].y, ax1 = ax + list[0].width, ay1 = ay + list[0].height;
        if (displayArg == "all") {
            for (const scr::Display& d : list) {
                ax = std::min(ax, d.x);
                ay = std::min(ay, d.y);
                ax1 = std::max(ax1, d.x + d.width);
                ay1 = std::max(ay1, d.y + d.height);
            }
        } else {
            const scr::Display* pick = nullptr;
            for (const scr::Display& d : list)
                if (displayArg == "primary" ? d.primary : d.index == displayIndex) pick = &d;
            if (!pick && displayArg == "primary") pick = &list[0];
            if (!pick)
                return rejected("no display " + std::to_string(displayIndex) + " (captureDisplays() lists " +
                                    std::to_string(list.size()) + ")",
                                "EINVAL");
            displayIndex = pick->index;
            ax = pick->x;
            ay = pick->y;
            ax1 = pick->x + pick->width;
            ay1 = pick->y + pick->height;
        }
        int gx = ax, gy = ay, gw = ax1 - ax, gh = ay1 - ay;
        if (hasRect) {
            const int x0 = std::max(ax, ax + static_cast<int>(std::floor(rx)));
            const int y0 = std::max(ay, ay + static_cast<int>(std::floor(ry)));
            const int x1 = std::min(ax1, ax + static_cast<int>(std::ceil(rx + rw)));
            const int y1 = std::min(ay1, ay + static_cast<int>(std::ceil(ry + rh)));
            if (x1 <= x0 || y1 <= y0) return rejected("rect is outside the display", "EINVAL");
            gx = x0;
            gy = y0;
            gw = x1 - x0;
            gh = y1 - y0;
        }

        scr::Grab g;
        const scr::Status st = eng->grabDesktop(gx, gy, gw, gh, g, why, cursor);
        if (st != scr::Status::Ok) {
            LOG_WARN("bro.window.captureScreen: %s", why.c_str());
            return rejected(why, codeFor(st));
        }
        if (!path.empty()) {
            if (!ensureParentDir(path) ||
                !broimage::encode_png_file(path, g.rgba.data(), g.width, g.height, 4))
                return rejected("writing " + path + " failed", "EIO");
        }

        ObjectBuilder res;
        res.set("x", ev::fromDouble(g.x));
        res.set("y", ev::fromDouble(g.y));
        res.set("width", ev::fromDouble(g.width));
        res.set("height", ev::fromDouble(g.height));
        res.set("display", ev::fromDouble(displayArg == "all" ? -1 : displayIndex));
        res.set("backend", ev::fromUtf8(g.backend));
        if (!path.empty()) res.set("path", ev::fromUtf8(path));
        if (wantPixels) {
            ev::Persistent px(makeUint8Array(g.rgba));
            res.set("pixels", px.get());
        }
        ev::Persistent promise(ev::createPromise());
        ev::resolvePromise(promise.get(), res.get());
        return promise.get();
    });
}

}  // namespace bro::bronze_host
