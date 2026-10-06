// HTMLTerminalElement: script's handle on a <terminal> (layout::ElTerminal).
// bro paints and feeds the element itself; script starts the child, writes
// to it, reads the screen, sets options and colours, and listens for the
// element's events. The control is created on first use, the way the layout
// pass creates it for markup, so a terminal made and spawned in one turn
// works before it has ever been laid out. docs/terminal-api.js is the
// contract. The view half (scrollback, selection, search, links, commands)
// is host_element_terminal_view.cpp.

#include "bronze_host/host_element_terminal.h"
#include "bronze_host/host_bro_namespaces.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"

#include "dom/element.h"
#include "engine/engine.h"
#include "engine/terminal_layers.h"
#include "layout/el_terminal.h"

#include <cmath>
#include <memory>
#include <string>

namespace bro::bronze_host {

layout::ElTerminal* hostTerminalControl(Value self, bool create) {
    HostNodeState* st = hostNodeStateOfValue(self);
    dom::Element* el = st ? st->el : nullptr;
    if (!el) return nullptr;
    if (auto* t = el->terminalControl()) return t;
    if (!create || (el->tagName() != "terminal" && el->tagName() != "TERMINAL")) return nullptr;
    auto* eng = hostEngine();
    if (!eng) return nullptr;
    auto ctrl = std::make_unique<layout::ElTerminal>(eng->renderer());
    ctrl->setElement(el);
    el->setTerminalControl(std::move(ctrl));
    return el->terminalControl();
}

namespace {

layout::ElTerminal* control(Value self, bool create = true) { return hostTerminalControl(self, create); }

std::string stringArg(std::span<const Value> a, size_t i) {
    return i < a.size() && !ev::isUndefined(a[i]) && !ev::isNull(a[i]) ? ev::toUtf8(a[i]) : std::string();
}

// A property of a rooted options object, if present (not undefined / null).
bool prop(const ev::Persistent& obj, const char* name, ev::Persistent& out) {
    out.set(ev::getProperty(obj.get(), name));
    return !ev::isUndefined(out.get()) && !ev::isNull(out.get());
}

// spawn()'s options object: { command, args, cwd, env }.
bool readSpawnSpec(Value opts, layout::ElTerminal::SpawnSpec& spec, std::string& error) {
    if (ev::isUndefined(opts) || ev::isNull(opts)) return true;
    if (!ev::isObject(opts)) {
        error = "spawn(options): options must be an object";
        return false;
    }
    ev::Persistent root(opts);
    Value cmd = ev::getProperty(root.get(), "command");
    if (!ev::isUndefined(cmd) && !ev::isNull(cmd)) spec.command = ev::toUtf8(cmd);
    Value cwd = ev::getProperty(root.get(), "cwd");
    if (!ev::isUndefined(cwd) && !ev::isNull(cwd)) spec.cwd = ev::toUtf8(cwd);

    ev::Persistent args(ev::getProperty(root.get(), "args"));
    if (!ev::isUndefined(args.get()) && !ev::isNull(args.get())) {
        if (!hostIsArray(args.get())) {
            error = "spawn(options): args must be an array of strings";
            return false;
        }
        uint32_t n = 0;
        if (!lengthWithin(ev::toDouble(ev::getProperty(args.get(), "length")), kMaxHostListLength, n)) {
            error = "spawn(options): too many args";
            return false;
        }
        for (uint32_t i = 0; i < n; ++i) spec.args.push_back(ev::toUtf8(ev::getElement(args.get(), i)));
    }

    // env: a plain object of NAME: value, read through Object.entries.
    ev::Persistent env(ev::getProperty(root.get(), "env"));
    if (!ev::isUndefined(env.get()) && !ev::isNull(env.get())) {
        if (!ev::isObject(env.get())) {
            error = "spawn(options): env must be an object";
            return false;
        }
        ev::GlobalValue object = ev::globalValue("Object");
        if (!object.found || !ev::isObject(object.value)) {
            error = "spawn(options): no Object global";
            return false;
        }
        ev::Persistent objectCtor(object.value);
        ev::Persistent entriesFn(ev::getProperty(objectCtor.get(), "entries"));
        if (!ev::isFunction(entriesFn.get())) {
            error = "spawn(options): no Object.entries";
            return false;
        }
        Value arg = env.get();
        ev::CallResult res = ev::call(entriesFn.get(), objectCtor.get(), std::span<const Value>(&arg, 1));
        if (res.thrown) {
            error = "spawn(options): env could not be read";
            return false;
        }
        ev::Persistent entries(res.value);
        uint32_t n = 0;
        if (!lengthWithin(ev::toDouble(ev::getProperty(entries.get(), "length")), kMaxHostListLength, n)) n = 0;
        for (uint32_t i = 0; i < n; ++i) {
            ev::Persistent pair(ev::getElement(entries.get(), i));
            std::string k = ev::toUtf8(ev::getElement(pair.get(), 0));
            std::string v = ev::toUtf8(ev::getElement(pair.get(), 1));
            spec.env.emplace_back(std::move(k), std::move(v));
        }
    }
    return true;
}

Value optionsValue(const layout::ElTerminal::Options& o) {
    ObjectBuilder b;
    b.set("copyOnSelect", ev::fromBool(o.copyOnSelect));
    b.set("middleClickPaste", ev::fromBool(o.middleClickPaste));
    b.set("scrollOnInput", ev::fromBool(o.scrollOnInput));
    b.set("boldIsBright", ev::fromBool(o.boldIsBright));
    b.set("minimumContrast", ev::fromDouble(o.minimumContrast));
    b.set("ligatures", ev::fromBool(o.ligatures));
    b.set("clipboard", ev::fromUtf8(o.clipboard));
    b.set("wheelLines", ev::fromDouble(o.wheelLines));
    b.set("imageMemoryLimit", ev::fromDouble(o.imageMemoryLimit));
    return b.get();
}

// Merges the keys `v` has over `o`. False with `error` on a bad value.
bool readOptions(Value v, layout::ElTerminal::Options& o, std::string& error) {
    if (!ev::isObject(v)) {
        error = "options must be an object";
        return false;
    }
    ev::Persistent obj(v);
    ev::Persistent p(ev::undefined());
    if (prop(obj, "copyOnSelect", p)) o.copyOnSelect = ev::toBool(p.get());
    if (prop(obj, "middleClickPaste", p)) o.middleClickPaste = ev::toBool(p.get());
    if (prop(obj, "scrollOnInput", p)) o.scrollOnInput = ev::toBool(p.get());
    if (prop(obj, "boldIsBright", p)) o.boldIsBright = ev::toBool(p.get());
    if (prop(obj, "ligatures", p)) o.ligatures = ev::toBool(p.get());
    if (prop(obj, "minimumContrast", p)) {
        const double d = ev::isObject(p.get()) ? 0.0 : ev::toDouble(p.get());
        if (!(d >= 1.0 && d <= 21.0)) {
            error = "minimumContrast must be a contrast ratio from 1 to 21";
            return false;
        }
        o.minimumContrast = float(d);
    }
    if (prop(obj, "wheelLines", p)) {
        const double d = ev::isObject(p.get()) ? 0.0 : ev::toDouble(p.get());
        if (!(d >= 1.0 && d <= 100.0)) {
            error = "wheelLines must be from 1 to 100";
            return false;
        }
        o.wheelLines = int(d);
    }
    if (prop(obj, "imageMemoryLimit", p)) {
        const double d = ev::isObject(p.get()) ? -1.0 : ev::toDouble(p.get());
        if (!(d >= 0.0 && d <= 4.0 * 1024 * 1024 * 1024)) {
            error = "imageMemoryLimit must be a byte count from 0 to 4 GiB";
            return false;
        }
        o.imageMemoryLimit = std::floor(d);
    }
    if (prop(obj, "clipboard", p)) {
        const std::string c = ev::toUtf8(p.get());
        if (c != "deny" && c != "write" && c != "read-write") {
            error = "clipboard must be \"deny\", \"write\" or \"read-write\"";
            return false;
        }
        o.clipboard = c;
    }
    return true;
}

Value themeValue(const layout::ElTerminal::Theme& t) {
    ObjectBuilder b;
    b.set("foreground", ev::fromUtf8(t.foreground));
    b.set("background", ev::fromUtf8(t.background));
    b.set("cursor", ev::fromUtf8(t.cursor));
    b.set("selection", ev::fromUtf8(t.selection));
    b.set("match", ev::fromUtf8(t.match));
    b.set("currentMatch", ev::fromUtf8(t.currentMatch));
    ev::Persistent ansi(hostArrayOf(t.ansi.size(), [&t](size_t i) { return ev::fromUtf8(t.ansi[i]); }));
    b.set("ansi", ansi.get());
    return b.get();
}

// A theme object: each key present replaces that slot ("" or null returns it
// to CSS); keys left out keep what script set before.
bool readTheme(Value v, layout::ElTerminal::Theme& t, std::string& error) {
    if (ev::isNull(v) || ev::isUndefined(v)) {
        t = layout::ElTerminal::Theme{};
        return true;
    }
    if (!ev::isObject(v)) {
        error = "theme must be an object";
        return false;
    }
    ev::Persistent obj(v);
    auto slot = [&](const char* name, std::string& out) {
        Value p = ev::getProperty(obj.get(), name);
        if (ev::isUndefined(p)) return;
        out = ev::isNull(p) ? std::string() : ev::toUtf8(p);
    };
    slot("foreground", t.foreground);
    slot("background", t.background);
    slot("cursor", t.cursor);
    slot("selection", t.selection);
    slot("match", t.match);
    slot("currentMatch", t.currentMatch);
    ev::Persistent ansi(ev::getProperty(obj.get(), "ansi"));
    if (!ev::isUndefined(ansi.get()) && !ev::isNull(ansi.get())) {
        // Index 0-15 the ANSI colours, 16-255 the rest of the 256-colour table;
        // a hole (or null) leaves that slot to CSS.
        if (!hostIsArray(ansi.get())) {
            error = "theme.ansi must be an array of up to 256 colours";
            return false;
        }
        uint32_t n = 0;
        if (!lengthWithin(ev::toDouble(ev::getProperty(ansi.get(), "length")), 256, n)) {
            error = "theme.ansi must be an array of up to 256 colours";
            return false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            Value c = ev::getElement(ansi.get(), i);
            t.ansi[i] = ev::isNull(c) || ev::isUndefined(c) ? std::string() : ev::toUtf8(c);
        }
    }
    return true;
}

}  // namespace

Value makeBroTerminalValue() {
    ObjectBuilder o;
    o.set("available", ev::fromBool(layout::ElTerminal::available()));
    // The shell spawn() starts when no command is given.
    o.accessor("defaultShell",
        [](Value, std::span<const Value>) -> Value { return ev::fromUtf8(layout::ElTerminal::defaultShell()); },
        nullptr);
    // Lifetime counters: how often the terminals' own layers and the page's
    // cached paint were recorded (the compositor-layer tests and the perf
    // probe read these).
    o.def("stats", 0, [](Value, std::span<const Value>) -> Value {
        ObjectBuilder s;
        // False under BRO_TERMINAL_LAYER=0: terminals paint inline with the page.
        s.set("layered", ev::fromBool(engine::terminalLayersEnabled()));
        s.set("layerRecords", ev::fromDouble(double(layout::ElTerminal::totalLayerRecords())));
        auto* eng = hostEngine();
        s.set("pageRecords", ev::fromDouble(eng ? double(eng->frameStats().baseRecords) : 0.0));
        s.set("pageInvalidations", ev::fromDouble(eng ? double(eng->frameStats().baseInvalidations) : 0.0));
        return s.get();
    });
    return o.get();
}

void decorateTerminalProto(ObjectBuilder& b) {
    // spawn({ command, args, cwd, env }) -> pid. Throws when the process
    // cannot be started, or when this terminal already has one.
    b.def("spawn", 1, [](Value self, std::span<const Value> a) -> Value {
        auto* t = control(self);
        if (!t) return ev::throwTypeError("spawn(): not a <terminal>");
        layout::ElTerminal::SpawnSpec spec;
        std::string error;
        if (!readSpawnSpec(a.empty() ? ev::undefined() : a[0], spec, error)) return ev::throwTypeError(error.c_str());
        if (!t->spawn(spec, &error))
            return ev::throwValue(hostMakeDomError("OperationError", "spawn(): " + error));
        return ev::fromDouble(double(t->pid()));
    });
    // write(data): raw bytes (a UTF-8 string) to the child's input.
    b.def("write", 1, [](Value self, std::span<const Value> a) -> Value {
        auto* t = control(self);
        return ev::fromBool(t && t->write(stringArg(a, 0)));
    });
    // feed(data): bytes into the emulator as if the child had written them.
    b.def("feed", 1, [](Value self, std::span<const Value> a) -> Value {
        if (auto* t = control(self)) t->feed(stringArg(a, 0));
        return ev::undefined();
    });
    // paste(text): as a paste from the clipboard (bracketed when asked for).
    b.def("paste", 1, [](Value self, std::span<const Value> a) -> Value {
        auto* t = control(self);
        return ev::fromBool(t && t->paste(stringArg(a, 0)));
    });
    b.def("kill", 0, [](Value self, std::span<const Value>) -> Value {
        if (auto* t = control(self, false)) t->kill();
        return ev::undefined();
    });
    b.def("screenText", 0, [](Value self, std::span<const Value>) -> Value {
        auto* t = control(self);
        return ev::fromUtf8(t ? t->screenText() : std::string());
    });
    b.def("scrollbackText", 0, [](Value self, std::span<const Value>) -> Value {
        auto* t = control(self);
        return ev::fromUtf8(t ? t->scrollbackText() : std::string());
    });
    b.def("frameText", 0, [](Value self, std::span<const Value>) -> Value {
        auto* t = control(self);
        return ev::fromUtf8(t ? t->frameText() : std::string());
    });

    auto readOnly = [&b](const char* name, Value (*get)(layout::ElTerminal&)) {
        b.accessor(name,
            [get](Value self, std::span<const Value>) -> Value {
                auto* t = control(self);
                return t ? get(*t) : ev::undefined();
            },
            nullptr);
    };
    readOnly("cols", [](layout::ElTerminal& t) { return ev::fromDouble(t.cols()); });
    readOnly("rows", [](layout::ElTerminal& t) { return ev::fromDouble(t.rows()); });
    readOnly("pid", [](layout::ElTerminal& t) { return ev::fromDouble(double(t.pid())); });
    readOnly("running", [](layout::ElTerminal& t) { return ev::fromBool(t.running()); });
    readOnly("exitCode", [](layout::ElTerminal& t) {
        auto c = t.exitCode();
        return c ? ev::fromDouble(*c) : ev::null();
    });
    readOnly("title", [](layout::ElTerminal& t) { return ev::fromUtf8(t.title()); });
    readOnly("cwd", [](layout::ElTerminal& t) { return ev::fromUtf8(t.cwd()); });
    readOnly("foregroundProcess", [](layout::ElTerminal& t) {
        const auto p = t.foregroundProcess();
        if (!p) return ev::null();
        ObjectBuilder o;
        o.set("pid", ev::fromDouble(double(p->pid)));
        o.set("name", ev::fromUtf8(p->name));
        o.set("path", ev::fromUtf8(p->path));
        o.set("commandLine", ev::fromUtf8(p->commandLine));
        return o.get();
    });
    readOnly("pointerShape", [](layout::ElTerminal& t) { return ev::fromUtf8(t.pointerShape()); });
    readOnly("cursor", [](layout::ElTerminal& t) {
        const auto c = t.cursor();
        ObjectBuilder o;
        o.set("row", ev::fromDouble(c.row));
        o.set("col", ev::fromDouble(c.col));
        o.set("visible", ev::fromBool(c.visible));
        o.set("blink", ev::fromBool(c.blink));
        o.set("shape", ev::fromUtf8(c.shape));
        return o.get();
    });
    readOnly("metrics", [](layout::ElTerminal& t) {
        const auto m = t.metrics();
        ObjectBuilder o;
        o.set("cellWidth", ev::fromDouble(m.cellWidth));
        o.set("cellHeight", ev::fromDouble(m.cellHeight));
        o.set("baseline", ev::fromDouble(m.baseline));
        o.set("pixelWidth", ev::fromDouble(m.pixelWidth));
        o.set("pixelHeight", ev::fromDouble(m.pixelHeight));
        o.set("scale", ev::fromDouble(m.scale));
        return o.get();
    });
    readOnly("layerRecords", [](layout::ElTerminal& t) { return ev::fromDouble(double(t.layerRecords())); });
    readOnly("images", [](layout::ElTerminal& t) {
        const auto s = t.images();
        ObjectBuilder o;
        o.set("count", ev::fromDouble(s.count));
        o.set("placements", ev::fromDouble(s.placements));
        o.set("bytes", ev::fromDouble(s.bytes));
        o.set("limit", ev::fromDouble(s.limit));
        return o.get();
    });

    // options: read as a fresh object; assigning merges the keys given.
    b.accessor("options",
        [](Value self, std::span<const Value>) -> Value {
            auto* t = control(self);
            return t ? optionsValue(t->options()) : ev::undefined();
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto* t = control(self);
            if (!t) return ev::undefined();
            layout::ElTerminal::Options o = t->options();
            std::string error;
            if (!readOptions(argAt(a, 0), o, error)) return ev::throwTypeError(error.c_str());
            t->setOptions(o);
            return ev::undefined();
        });
    // theme: the colours in effect; assigning sets script's own (null: clear).
    b.accessor("theme",
        [](Value self, std::span<const Value>) -> Value {
            auto* t = control(self);
            return t ? themeValue(t->theme()) : ev::undefined();
        },
        [](Value self, std::span<const Value> a) -> Value {
            auto* t = control(self);
            if (!t) return ev::undefined();
            layout::ElTerminal::Theme theme = t->scriptTheme();
            std::string error;
            if (!readTheme(argAt(a, 0), theme, error)) return ev::throwTypeError(error.c_str());
            t->setTheme(theme);
            return ev::undefined();
        });

    decorateTerminalViewProto(b);
}

}  // namespace bro::bronze_host
