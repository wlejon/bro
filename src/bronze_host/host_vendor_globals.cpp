// Vendor globals loaded via <script> tags in Three.js Editor:
// signals, CodeMirror, acorn, tern, esprima, jsonlint, draco_encoder.
//
// These provide native Bronze implementations and fallbacks for vendor
// libraries loaded by the host environment / page, allowing compiled ESM
// modules to reference them seamlessly as host globals.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// JS-Signals (signals.Signal)
// ---------------------------------------------------------------------------

struct SignalBinding {
    ev::Persistent fn;
    ev::Persistent context;
    bool once = false;
};

struct HostSignal {
    std::vector<SignalBinding> bindings;
    bool active = true;
    bool shouldPropagate = true;
};

void hostSignalDtor(void* p) {
    delete static_cast<HostSignal*>(p);
}

HostSignal* signalOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    return static_cast<HostSignal*>(ev::handleData(v));
}

Value makeSignalInstance() {
    auto* sig = new HostSignal();
    ObjectBuilder b(ev::makeHandle(sig, hostSignalDtor));

    b.def("add", 3, [sig](Value, std::span<const Value> a) {
        Value fn = argAt(a, 0);
        if (!ev::isFunction(fn)) return ev::throwTypeError("Signal.add: listener must be a function");
        Value ctx = argAt(a, 1);
        sig->bindings.push_back({ev::Persistent(fn), ev::Persistent(ctx), false});
        return ev::undefined();
    });

    b.def("addOnce", 3, [sig](Value, std::span<const Value> a) {
        Value fn = argAt(a, 0);
        if (!ev::isFunction(fn)) return ev::throwTypeError("Signal.addOnce: listener must be a function");
        Value ctx = argAt(a, 1);
        sig->bindings.push_back({ev::Persistent(fn), ev::Persistent(ctx), true});
        return ev::undefined();
    });

    b.def("remove", 2, [sig](Value, std::span<const Value> a) {
        Value fn = argAt(a, 0);
        if (!ev::isFunction(fn)) return ev::undefined();
        Value ctx = argAt(a, 1);
        auto& list = sig->bindings;
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (ev::toBits(it->fn.get()) == ev::toBits(fn)) {
                if (ev::isUndefined(ctx) || ev::isNull(ctx) ||
                    ev::toBits(it->context.get()) == ev::toBits(ctx)) {
                    list.erase(it);
                    break;
                }
            }
        }
        return ev::undefined();
    });

    b.def("removeAll", 0, [sig](Value, std::span<const Value>) {
        sig->bindings.clear();
        return ev::undefined();
    });

    b.def("has", 2, [sig](Value, std::span<const Value> a) {
        Value fn = argAt(a, 0);
        if (!ev::isFunction(fn)) return ev::fromBool(false);
        Value ctx = argAt(a, 1);
        for (const auto& bnd : sig->bindings) {
            if (ev::toBits(bnd.fn.get()) == ev::toBits(fn)) {
                if (ev::isUndefined(ctx) || ev::isNull(ctx) ||
                    ev::toBits(bnd.context.get()) == ev::toBits(ctx)) {
                    return ev::fromBool(true);
                }
            }
        }
        return ev::fromBool(false);
    });

    b.def("getNumListeners", 0, [sig](Value, std::span<const Value>) {
        return ev::fromDouble(static_cast<double>(sig->bindings.size()));
    });

    b.def("halt", 0, [sig](Value, std::span<const Value>) {
        sig->shouldPropagate = false;
        return ev::undefined();
    });

    b.def("dispatch", 0, [sig](Value, std::span<const Value> a) {
        if (!sig->active || sig->bindings.empty()) return ev::undefined();
        sig->shouldPropagate = true;
        // Snapshot to allow listeners to modify bindings during dispatch
        std::vector<SignalBinding> current = sig->bindings;
        for (const auto& bnd : current) {
            if (!sig->shouldPropagate) break;
            if (bnd.once) {
                for (auto it = sig->bindings.begin(); it != sig->bindings.end(); ++it) {
                    if (ev::toBits(it->fn.get()) == ev::toBits(bnd.fn.get())) {
                        sig->bindings.erase(it);
                        break;
                    }
                }
            }
            Value receiver = ev::isUndefined(bnd.context.get()) ? ev::undefined() : bnd.context.get();
            ev::CallResult r = ev::call(bnd.fn.get(), receiver, a);
            if (r.thrown) reportBronzeError("Signal.dispatch", r.value);
        }
        return ev::undefined();
    });

    b.def("dispose", 0, [sig](Value, std::span<const Value>) {
        sig->bindings.clear();
        sig->active = false;
        return ev::undefined();
    });

    b.accessor("active",
               [sig](Value, std::span<const Value>) { return ev::fromBool(sig->active); },
               [sig](Value, std::span<const Value> a) {
                   sig->active = ev::toBool(argAt(a, 0));
                   return ev::undefined();
               });

    b.set("memorize", ev::fromBool(false));
    return b.get();
}

Value makeSignalsValue() {
    Value signalCtor = ev::makeFunction(
        [](Value, std::span<const Value>) {
            return makeSignalInstance();
        },
        0);

    ObjectBuilder b;
    b.set("Signal", signalCtor);
    return b.get();
}

// ---------------------------------------------------------------------------
// CodeMirror
// ---------------------------------------------------------------------------

Value makeCodeMirrorInstance(Value containerVal, Value optionsVal) {
    ObjectBuilder b;
    std::string textContent = "";
    if (ev::isObject(optionsVal)) {
        Value valProp = ev::getProperty(optionsVal, "value");
        if (!ev::isUndefined(valProp) && !ev::isObject(valProp)) {
            textContent = ev::toUtf8(valProp);
        }
    }

    auto contentHolder = std::make_shared<std::string>(textContent);

    b.def("getValue", 0, [contentHolder](Value, std::span<const Value>) {
        return ev::fromUtf8(*contentHolder);
    });

    b.def("setValue", 1, [contentHolder](Value, std::span<const Value> a) {
        Value v = argAt(a, 0);
        *contentHolder = (!ev::isObject(v) && !ev::isUndefined(v)) ? ev::toUtf8(v) : "";
        return ev::undefined();
    });

    b.def("setOption", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
    b.def("getOption", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
    b.def("on", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
    b.def("off", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
    b.def("focus", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
    b.def("refresh", 0, [](Value, std::span<const Value>) { return ev::undefined(); });

    ev::Persistent containerPersistent(containerVal);
    b.def("getWrapperElement", 0, [containerPersistent](Value, std::span<const Value>) {
        Value c = containerPersistent.get();
        if (ev::isObject(c)) return c;
        ObjectBuilder wrap;
        return wrap.get();
    });

    b.def("getScrollerElement", 0, [](Value, std::span<const Value>) {
        ObjectBuilder scroller;
        return scroller.get();
    });

    {
        ObjectBuilder st;
        st.set("focused", ev::fromBool(false));
        b.set("state", st.get());
    }

    b.def("getCursor", 0, [](Value, std::span<const Value>) {
        ObjectBuilder cur;
        cur.set("line", ev::fromDouble(0));
        cur.set("ch", ev::fromDouble(0));
        return cur.get();
    });

    b.def("setCursor", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
    b.def("getSelection", 0, [](Value, std::span<const Value>) { return ev::fromUtf8(""); });
    b.def("replaceSelection", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
    b.def("operation", 1, [](Value, std::span<const Value> a) {
        Value fn = argAt(a, 0);
        if (ev::isFunction(fn)) {
            ev::call(fn, ev::undefined(), std::span<const Value>());
        }
        return ev::undefined();
    });
    b.def("showHint", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
    b.def("setSelection", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
    b.def("addLineClass", 3, [](Value, std::span<const Value>) { return ev::undefined(); });
    b.def("removeLineClass", 3, [](Value, std::span<const Value>) { return ev::undefined(); });
    b.def("addLineWidget", 3, [](Value, std::span<const Value>) {
        ObjectBuilder w;
        w.def("clear", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
        return w.get();
    });
    b.def("clearGutter", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
    b.def("setGutterMarker", 3, [](Value, std::span<const Value>) { return ev::undefined(); });
    b.def("getLine", 1, [](Value, std::span<const Value>) { return ev::fromUtf8(""); });
    b.def("lineCount", 0, [](Value, std::span<const Value>) { return ev::fromDouble(1); });

    return b.get();
}

Value makeCodeMirrorValue() {
    Value cmFn = ev::makeFunction(
        [](Value, std::span<const Value> a) {
            Value container = argAt(a, 0);
            Value options = argAt(a, 1);
            return makeCodeMirrorInstance(container, options);
        },
        2);

    ev::Persistent cmP(cmFn);
    {
        ObjectBuilder pass;
        cmP.set(ev::setProperty(cmP.get(), "Pass", pass.get()));
    }
    {
        ObjectBuilder cmd;
        cmP.set(ev::setProperty(cmP.get(), "commands", cmd.get()));
    }
    cmP.set(ev::setProperty(cmP.get(), "defineMode", ev::makeFunction([](Value, std::span<const Value>) { return ev::undefined(); }, 2)));
    cmP.set(ev::setProperty(cmP.get(), "defineExtension", ev::makeFunction([](Value, std::span<const Value>) { return ev::undefined(); }, 2)));
    cmP.set(ev::setProperty(cmP.get(), "registerHelper", ev::makeFunction([](Value, std::span<const Value>) { return ev::undefined(); }, 3)));
    cmP.set(ev::setProperty(cmP.get(), "Pos", ev::makeFunction([](Value, std::span<const Value> a) {
        ObjectBuilder pos;
        pos.set("line", argAt(a, 0));
        pos.set("ch", argAt(a, 1));
        return pos.get();
    }, 2)));
    cmP.set(ev::setProperty(cmP.get(), "fromTextArea", ev::makeFunction([](Value, std::span<const Value> a) {
        return makeCodeMirrorInstance(argAt(a, 0), argAt(a, 1));
    }, 2)));

    Value ternServerCtor = ev::makeFunction(
        [](Value, std::span<const Value>) {
            ObjectBuilder s;
            s.def("addDoc", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
            s.def("delDoc", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
            s.def("hideDoc", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
            s.def("complete", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
            s.def("showType", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
            s.def("showDocs", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
            s.def("jumpToDef", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
            s.def("jumpBack", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
            s.def("rename", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
            s.def("selectName", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
            {
                ObjectBuilder srv;
                srv.def("addDoc", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
                srv.def("delDoc", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
                srv.def("reset", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
                srv.set("defs", hostArrayOf(0, [](size_t) { return ev::undefined(); }));
                s.set("server", srv.get());
            }
            return s.get();
        },
        1);
    cmP.set(ev::setProperty(cmP.get(), "TernServer", ternServerCtor));

    return cmP.get();
}

// ---------------------------------------------------------------------------
// Acorn, Esprima, Tern, JsonLint, Draco
// ---------------------------------------------------------------------------

Value makeAcornValue() {
    ObjectBuilder b;
    auto makeProgram = []() {
        ObjectBuilder prog;
        prog.set("type", ev::fromUtf8("Program"));
        prog.set("sourceType", ev::fromUtf8("script"));
        prog.set("body", hostArrayOf(0, [](size_t) { return ev::undefined(); }));
        return prog.get();
    };

    b.def("parse", 2, [makeProgram](Value, std::span<const Value>) {
        return makeProgram();
    });
    b.def("parse_dammit", 2, [makeProgram](Value, std::span<const Value>) {
        return makeProgram();
    });

    {
        ObjectBuilder walk;
        walk.def("simple", 3, [](Value, std::span<const Value>) { return ev::undefined(); });
        walk.def("ancestor", 3, [](Value, std::span<const Value>) { return ev::undefined(); });
        walk.def("recursive", 4, [](Value, std::span<const Value>) { return ev::undefined(); });
        b.set("walk", walk.get());
    }
    {
        ObjectBuilder opts;
        opts.set("ecmaVersion", ev::fromDouble(2022));
        b.set("defaultOptions", opts.get());
    }
    return b.get();
}

Value makeEsprimaValue() {
    ObjectBuilder b;
    b.def("parse", 2, [](Value, std::span<const Value>) {
        ObjectBuilder prog;
        prog.set("type", ev::fromUtf8("Program"));
        prog.set("sourceType", ev::fromUtf8("script"));
        prog.set("body", hostArrayOf(0, [](size_t) { return ev::undefined(); }));
        return prog.get();
    });
    {
        ObjectBuilder syn;
        syn.set("Program", ev::fromUtf8("Program"));
        b.set("Syntax", syn.get());
    }
    return b.get();
}

Value makeTernValue() {
    ObjectBuilder b;
    Value serverCtor = ev::makeFunction([](Value, std::span<const Value>) {
        ObjectBuilder s;
        s.def("addDoc", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
        s.def("delDoc", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
        s.def("reset", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
        s.set("defs", hostArrayOf(0, [](size_t) { return ev::undefined(); }));
        s.def("request", 2, [](Value, std::span<const Value> a) {
            Value cb = argAt(a, 1);
            if (ev::isFunction(cb)) {
                Value err = ev::null();
                Value res = ev::null();
                Value args[2] = {err, res};
                ev::call(cb, ev::undefined(), std::span<const Value>(args, 2));
            }
            return ev::undefined();
        });
        s.def("flush", 1, [](Value, std::span<const Value> a) {
            Value cb = argAt(a, 0);
            if (ev::isFunction(cb)) {
                ev::call(cb, ev::undefined(), std::span<const Value>());
            }
            return ev::undefined();
        });
        return s.get();
    }, 1);

    b.set("Server", serverCtor);
    b.def("defineQueryType", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
    return b.get();
}

Value makeJsonlintValue() {
    ObjectBuilder b;
    b.def("parse", 1, [](Value, std::span<const Value> a) {
        Value strV = argAt(a, 0);
        if (ev::isObject(strV) || ev::isUndefined(strV)) {
            return ev::throwError("jsonlint: argument must be a string");
        }
        std::string s = ev::toUtf8(strV);
        if (s.empty()) return ev::null();
        // Minimal parse or brand return
        ObjectBuilder res;
        return res.get();
    });
    return b.get();
}

Value makeDracoEncoderValue() {
    ObjectBuilder b;
    b.def("Encoder", 0, [](Value, std::span<const Value>) {
        ObjectBuilder enc;
        enc.def("SetSpeedOptions", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
        enc.def("SetAttributeQuantization", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
        enc.def("SetEncodingMethod", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
        enc.def("EncodeMeshToDracoBuffer", 2, [](Value, std::span<const Value>) { return ev::fromDouble(0); });
        return enc.get();
    });
    b.def("Mesh", 0, [](Value, std::span<const Value>) {
        ObjectBuilder mesh;
        mesh.def("AddAppendedFloatAttribute", 4, [](Value, std::span<const Value>) { return ev::fromDouble(0); });
        mesh.def("AddFacesToMesh", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
        mesh.def("set_num_points", 1, [](Value, std::span<const Value>) { return ev::undefined(); });
        return mesh.get();
    });
    b.def("DracoInt8Array", 0, [](Value, std::span<const Value>) {
        ObjectBuilder arr;
        arr.def("size", 0, [](Value, std::span<const Value>) { return ev::fromDouble(0); });
        arr.def("GetValue", 1, [](Value, std::span<const Value>) { return ev::fromDouble(0); });
        return arr.get();
    });
    return b.get();
}

}  // namespace

void installVendorGlobals() {
    ev::registerGlobal("signals", makeSignalsValue());
    ev::registerGlobal("CodeMirror", makeCodeMirrorValue());
    ev::registerGlobal("acorn", makeAcornValue());
    ev::registerGlobal("tern", makeTernValue());
    ev::registerGlobal("esprima", makeEsprimaValue());
    ev::registerGlobal("jsonlint", makeJsonlintValue());
    ev::registerGlobal("draco_encoder", makeDracoEncoderValue());
}

}  // namespace bro::bronze_host
