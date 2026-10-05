// HTMLTerminalElement: script's handle on a <terminal> (layout::ElTerminal).
// bro paints and feeds the element itself; script starts the child, writes
// to it, reads the screen and listens for `resize` and `exit`. The control is
// created on first use, the way the layout pass creates it for markup, so a
// terminal made and spawned in one turn works before it has ever been laid
// out. docs/terminal-api.js is the contract.

#include "bronze_host/host_element_terminal.h"
#include "bronze_host/host_bro_namespaces.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"

#include "dom/element.h"
#include "engine/engine.h"
#include "layout/el_terminal.h"

#include <memory>
#include <string>

namespace bro::bronze_host {

namespace {

dom::Element* getElement(Value self) {
    HostNodeState* st = hostNodeStateOfValue(self);
    return st ? st->el : nullptr;
}

layout::ElTerminal* control(Value self, bool create = true) {
    dom::Element* el = getElement(self);
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

std::string stringArg(std::span<const Value> a, size_t i) {
    return i < a.size() && !ev::isUndefined(a[i]) && !ev::isNull(a[i]) ? ev::toUtf8(a[i]) : std::string();
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

}  // namespace

Value makeBroTerminalValue() {
    ObjectBuilder o;
    o.set("available", ev::fromBool(layout::ElTerminal::available()));
    // The shell spawn() starts when no command is given.
    o.accessor("defaultShell",
        [](Value, std::span<const Value>) -> Value { return ev::fromUtf8(layout::ElTerminal::defaultShell()); },
        nullptr);
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
}

}  // namespace bro::bronze_host
