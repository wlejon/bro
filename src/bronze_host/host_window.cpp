#include "bronze_host/bronze_host.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_window_open.h"
#include "engine/engine.h"
#include "platform/sdl_window.h"

#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_power.h>
#include <limits>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

static bool isHeadless() {
    auto* eng = hostEngine();
    return !eng || eng->displayMode() == engine::DisplayMode::Headless;
}

static platform::Window* getWindow() {
    auto* eng = hostEngine();
    return eng ? eng->window() : nullptr;
}

} // namespace

Value makeBatterySnapshotValue() {
    const double inf = std::numeric_limits<double>::infinity();
    bool charging = true;
    double chargingTime = 0.0;
    double dischargingTime = inf;
    double level = 1.0;

    if (!isHeadless()) {
        int secs = 0, pct = 0;
        switch (SDL_GetPowerInfo(&secs, &pct)) {
            case SDL_POWERSTATE_ON_BATTERY:
                charging = false;
                chargingTime = inf;
                dischargingTime = secs >= 0 ? static_cast<double>(secs) : inf;
                if (pct >= 0) level = pct / 100.0;
                break;
            case SDL_POWERSTATE_CHARGING:
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
                break;
        }
    }

    ObjectBuilder bat;
    bat.set("charging", ev::fromBool(charging));
    bat.set("chargingTime", ev::fromDouble(chargingTime));
    bat.set("dischargingTime", ev::fromDouble(dischargingTime));
    bat.set("level", ev::fromDouble(level));
    bat.def("addEventListener", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
    bat.def("removeEventListener", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
    bat.set("onchargingchange", ev::null());
    bat.set("onchargingtimechange", ev::null());
    bat.set("ondischargingtimechange", ev::null());
    bat.set("onlevelchange", ev::null());

    ev::Persistent p{ev::createPromise()};
    ev::resolvePromise(p.get(), bat.get());
    return p.get();
}

Value makeBroWindowValue() {
    ObjectBuilder win;

    // State accessor ('normal', 'minimized', 'maximized', 'fullscreen')
    win.accessor("state", [](Value, std::span<const Value>) -> Value {
        auto* w = getWindow();
        const char* state = "normal";
        if (w) {
            if (w->isMinimized()) state = "minimized";
            else if (w->isFullscreen()) state = "fullscreen";
            else if (w->isMaximized()) state = "maximized";
        }
        return ev::fromUtf8(state);
    }, nullptr);

    // borderless (getter / setter)
    win.accessor("borderless",
        [](Value, std::span<const Value>) -> Value {
            auto* w = getWindow();
            return ev::fromBool(w && w->isBorderless());
        },
        [](Value, std::span<const Value> a) -> Value {
            auto* w = getWindow();
            if (w && !a.empty()) {
                w->setBorderless(ev::toBool(a[0]));
            }
            return ev::undefined();
        });

    // alwaysOnTop (getter / setter)
    win.accessor("alwaysOnTop",
        [](Value, std::span<const Value>) -> Value {
            auto* w = getWindow();
            return ev::fromBool(w && w->isAlwaysOnTop());
        },
        [](Value, std::span<const Value> a) -> Value {
            auto* w = getWindow();
            if (w && !a.empty()) {
                w->setAlwaysOnTop(ev::toBool(a[0]));
            }
            return ev::undefined();
        });

    // Operations that no-op in headless
    win.def("minimize", 0, [](Value, std::span<const Value>) -> Value {
        if (!isHeadless()) {
            if (auto* w = getWindow()) w->minimize();
        }
        return ev::undefined();
    });

    win.def("maximize", 0, [](Value, std::span<const Value>) -> Value {
        if (!isHeadless()) {
            if (auto* w = getWindow()) w->maximize();
        }
        return ev::undefined();
    });

    win.def("restore", 0, [](Value, std::span<const Value>) -> Value {
        if (!isHeadless()) {
            if (auto* w = getWindow()) w->restore();
        }
        return ev::undefined();
    });

    win.def("getPosition", 0, [](Value, std::span<const Value>) -> Value {
        int x = 0, y = 0;
        if (auto* w = getWindow()) w->getPosition(x, y);
        ObjectBuilder pos;
        pos.set("x", ev::fromDouble(x));
        pos.set("y", ev::fromDouble(y));
        return pos.get();
    });

    win.def("setPosition", 2, [](Value, std::span<const Value> a) -> Value {
        if (!isHeadless() && a.size() >= 2) {
            if (auto* w = getWindow()) {
                int x = static_cast<int>(ev::toDouble(a[0]));
                int y = static_cast<int>(ev::toDouble(a[1]));
                w->setPosition(x, y);
            }
        }
        return ev::undefined();
    });

    win.def("getMinSize", 0, [](Value, std::span<const Value>) -> Value {
        int w = 0, h = 0;
        if (auto* win = getWindow()) win->getMinimumSize(w, h);
        ObjectBuilder sz;
        sz.set("width", ev::fromDouble(w));
        sz.set("height", ev::fromDouble(h));
        return sz.get();
    });

    win.def("setMinSize", 2, [](Value, std::span<const Value> a) -> Value {
        if (a.size() >= 2) {
            if (auto* win = getWindow()) {
                int w = static_cast<int>(ev::toDouble(a[0]));
                int h = static_cast<int>(ev::toDouble(a[1]));
                win->setMinimumSize(w, h);
            }
        }
        return ev::undefined();
    });

    win.def("getMaxSize", 0, [](Value, std::span<const Value>) -> Value {
        int w = 0, h = 0;
        if (auto* win = getWindow()) win->getMaximumSize(w, h);
        ObjectBuilder sz;
        sz.set("width", ev::fromDouble(w));
        sz.set("height", ev::fromDouble(h));
        return sz.get();
    });

    win.def("setMaxSize", 2, [](Value, std::span<const Value> a) -> Value {
        if (a.size() >= 2) {
            if (auto* win = getWindow()) {
                int w = static_cast<int>(ev::toDouble(a[0]));
                int h = static_cast<int>(ev::toDouble(a[1]));
                win->setMaximumSize(w, h);
            }
        }
        return ev::undefined();
    });

    win.def("getDisplays", 0, [](Value, std::span<const Value>) -> Value {
        auto* w = getWindow();
        if (!w) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        auto displays = w->getDisplays();
        return hostArrayOf(displays.size(), [&displays](size_t i) -> Value {
            const auto& d = displays[i];
            ObjectBuilder dobj;
            dobj.set("id", ev::fromDouble(d.id));
            dobj.set("name", ev::fromUtf8(d.name));
            {
                ObjectBuilder b;
                b.set("x", ev::fromDouble(d.x));
                b.set("y", ev::fromDouble(d.y));
                b.set("width", ev::fromDouble(d.width));
                b.set("height", ev::fromDouble(d.height));
                dobj.set("bounds", b.get());
            }
            {
                ObjectBuilder wa;
                wa.set("x", ev::fromDouble(d.workX));
                wa.set("y", ev::fromDouble(d.workY));
                wa.set("width", ev::fromDouble(d.workWidth));
                wa.set("height", ev::fromDouble(d.workHeight));
                dobj.set("workArea", wa.get());
            }
            dobj.set("refreshRate", ev::fromDouble(d.refreshRate));
            dobj.set("contentScale", ev::fromDouble(d.contentScale));
            dobj.set("isPrimary", ev::fromBool(d.isPrimary));
            dobj.set("isCurrent", ev::fromBool(d.isCurrent));
            return dobj.get();
        });
    });

    win.def("moveToDisplay", 1, [](Value, std::span<const Value> a) -> Value {
        auto* w = getWindow();
        if (!w || isHeadless() || a.empty()) return ev::fromBool(false);
        uint32_t id = static_cast<uint32_t>(ev::toDouble(a[0]));
        return ev::fromBool(w->moveToDisplay(id));
    });

    Value winVal = win.get();
    installBroWindowOpen(winVal);
    return winVal;
}

} // namespace bro::bronze_host
