#include "bronze_host/host_touch.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "dom/element.h"

namespace bro::bronze_host {

HostClass g_touchClass;
HostClass g_touchListClass;
HostClass g_touchEventClass;
HostClass g_gestureEventClass;

namespace {

// The initializer readers the W3C dictionaries want: a missing member is the
// declared default, never a throw, so `new TouchEvent('touchstart')` with no
// second argument is as legal as the full form.
double optNum(Value opts, const char* key, double dflt) {
    if (!ev::isObject(opts)) return dflt;
    Value v = ev::getProperty(opts, key);
    if (ev::isUndefined(v) || ev::isNull(v)) return dflt;
    return ev::toDouble(v);
}

bool optFlag(Value opts, const char* key) {
    if (!ev::isObject(opts)) return false;
    Value v = ev::getProperty(opts, key);
    return ev::toBool(v);
}

Value optRef(Value opts, const char* key) {
    if (!ev::isObject(opts)) return ev::null();
    Value v = ev::getProperty(opts, key);
    return ev::isUndefined(v) ? ev::null() : v;
}

// An empty TouchList, for the three lists a TouchEvent initializer left out.
// `touches` is not optional on the web in the sense that reading it must
// answer a list — code does `e.touches.length` without a guard.
Value emptyTouchList() {
    Value listObj = g_touchListClass.make(nullptr, [](void*) {});
    ev::Persistent root(listObj);
    Value zero = ev::fromDouble(0);
    ev::setProperty(root.get(), "length", zero);
    return root.get();
}

// A TouchList out of whatever the initializer gave for one of the three
// lists: an array (what a program writes), an existing TouchList (what a
// dispatched event carries), or nothing.
Value touchListFrom(Value opts, const char* key) {
    Value v = optRef(opts, key);
    if (!ev::isObject(v)) return emptyTouchList();
    ev::Persistent src(v);
    Value lenV = ev::getProperty(src.get(), "length");
    const uint32_t len = ev::isUndefined(lenV)
                             ? 0u
                             : static_cast<uint32_t>(static_cast<int64_t>(ev::toDouble(lenV)));
    Value listObj = g_touchListClass.make(nullptr, [](void*) {});
    ev::Persistent root(listObj);
    for (uint32_t i = 0; i < len; ++i) {
        Value item = ev::getElement(src.get(), i);
        ev::setElement(root.get(), i, item);
    }
    Value lenOut = ev::fromDouble(len);
    ev::setProperty(root.get(), "length", lenOut);
    return root.get();
}

// The members every constructed event carries, whatever its class: the three
// the dispatchEvent reader looks for plus the ones a listener reads without
// asking whether the event was synthesised.
void initEventMembers(ev::Persistent& self, const std::string& type,
                      ev::Persistent& opts) {
    Value typeV = ev::fromUtf8(type);
    ev::setProperty(self.get(), "type", typeV);
    Value bub = ev::fromBool(optFlag(opts.get(), "bubbles"));
    ev::setProperty(self.get(), "bubbles", bub);
    Value can = ev::fromBool(optFlag(opts.get(), "cancelable"));
    ev::setProperty(self.get(), "cancelable", can);
    Value comp = ev::fromBool(optFlag(opts.get(), "composed"));
    ev::setProperty(self.get(), "composed", comp);
    Value dp = ev::fromBool(false);
    ev::setProperty(self.get(), "defaultPrevented", dp);
    Value trusted = ev::fromBool(false);
    ev::setProperty(self.get(), "isTrusted", trusted);
    Value tgt = ev::null();
    ev::setProperty(self.get(), "target", tgt);
    Value cur = ev::null();
    ev::setProperty(self.get(), "currentTarget", cur);
    Value phase = ev::fromDouble(0);
    ev::setProperty(self.get(), "eventPhase", phase);
}

Value touchConstructor(Value self, std::span<const Value> a) {
    ev::Persistent me(self);
    ev::Persistent opts(argAt(a, 0));
    static const char* kNums[] = {"identifier", "clientX",  "clientY", "screenX",
                                  "screenY",    "pageX",    "pageY",   "radiusX",
                                  "radiusY",    "rotationAngle", "force"};
    for (const char* key : kNums) {
        Value v = ev::fromDouble(optNum(opts.get(), key, 0.0));
        ev::setProperty(me.get(), key, v);
    }
    Value target = optRef(opts.get(), "target");
    ev::setProperty(me.get(), "target", target);
    return me.get();
}

Value touchListConstructor(Value self, std::span<const Value> a) {
    ev::Persistent me(self);
    Value src = argAt(a, 0);
    uint32_t len = 0;
    if (ev::isObject(src)) {
        ev::Persistent srcRoot(src);
        Value lenV = ev::getProperty(srcRoot.get(), "length");
        len = ev::isUndefined(lenV)
                  ? 0u
                  : static_cast<uint32_t>(static_cast<int64_t>(ev::toDouble(lenV)));
        for (uint32_t i = 0; i < len; ++i) {
            Value item = ev::getElement(srcRoot.get(), i);
            ev::setElement(me.get(), i, item);
        }
    }
    Value lenOut = ev::fromDouble(len);
    ev::setProperty(me.get(), "length", lenOut);
    return me.get();
}

// The event type argument, read BEFORE anything allocates: `a` is a span of
// plain Values and a moving collection leaves every one of them stale.
std::string typeArg(std::span<const Value> a) {
    Value t = argAt(a, 0);
    if (ev::isUndefined(t)) return std::string();
    return ev::toUtf8(t);
}

Value touchEventConstructor(Value self, std::span<const Value> a) {
    ev::Persistent me(self);
    ev::Persistent opts(argAt(a, 1));
    const std::string type = typeArg(a);
    initEventMembers(me, type, opts);
    static const char* kLists[] = {"touches", "targetTouches", "changedTouches"};
    for (const char* key : kLists) {
        Value list = touchListFrom(opts.get(), key);
        ev::setProperty(me.get(), key, list);
    }
    static const char* kMods[] = {"ctrlKey", "shiftKey", "altKey", "metaKey"};
    for (const char* key : kMods) {
        Value v = ev::fromBool(optFlag(opts.get(), key));
        ev::setProperty(me.get(), key, v);
    }
    Value detail = ev::fromDouble(optNum(opts.get(), "detail", 0.0));
    ev::setProperty(me.get(), "detail", detail);
    return me.get();
}

Value gestureEventConstructor(Value self, std::span<const Value> a) {
    ev::Persistent me(self);
    ev::Persistent opts(argAt(a, 1));
    const std::string type = typeArg(a);
    initEventMembers(me, type, opts);
    Value scale = ev::fromDouble(optNum(opts.get(), "scale", 1.0));
    ev::setProperty(me.get(), "scale", scale);
    Value rot = ev::fromDouble(optNum(opts.get(), "rotation", 0.0));
    ev::setProperty(me.get(), "rotation", rot);
    Value cx = ev::fromDouble(optNum(opts.get(), "clientX", 0.0));
    ev::setProperty(me.get(), "clientX", cx);
    Value cy = ev::fromDouble(optNum(opts.get(), "clientY", 0.0));
    ev::setProperty(me.get(), "clientY", cy);
    return me.get();
}

}  // namespace

void installTouchGlobals() {
    // Constructible, all four: the old stack shipped these as JS polyfill
    // classes (dom_polyfills.js) and a program that synthesises a touch —
    // a test harness, a library's own input emulation — calls
    // `new TouchEvent('touchstart', {touches: [new Touch({...})]})` and hands
    // the result to dispatchEvent. Installing them as brands only, which the
    // port did, turned every one of those calls into "not constructible".
    g_touchClass.install("Touch", 1, touchConstructor, [](ObjectBuilder&) {});

    g_touchListClass.install("TouchList", 0, touchListConstructor, [](ObjectBuilder& b) {
        b.def("item", 1, [](Value self, std::span<const Value> a) -> Value {
            int32_t idx = i32At(a, 0);
            if (idx < 0) return ev::null();
            Value item = ev::getElement(self, static_cast<uint32_t>(idx));
            return ev::isUndefined(item) ? ev::null() : item;
        });
    });

    g_touchEventClass.install("TouchEvent", 1, touchEventConstructor, [](ObjectBuilder&) {});
    g_gestureEventClass.install("GestureEvent", 1, gestureEventConstructor,
                                [](ObjectBuilder&) {});

    ev::GlobalValue baseClass = ev::globalValue("UIEvent");
    if (!baseClass.found || !ev::isFunction(baseClass.value)) {
        baseClass = ev::globalValue("Event");
    }
    if (baseClass.found && ev::isFunction(baseClass.value)) {
        // Rooted across the first setPrototype, which may allocate.
        ev::Persistent baseProto(ev::getProperty(baseClass.value, "prototype"));
        if (ev::isObject(baseProto.get())) {
            ev::setPrototype(g_touchEventClass.prototype(), baseProto.get());
            ev::setPrototype(g_gestureEventClass.prototype(), baseProto.get());
        }
    }
}

Value makeTouchValue(const dom::TouchPoint& pt) {
    Value touchObj = g_touchClass.make(nullptr, [](void*) {});
    ObjectBuilder b(touchObj);
    b.set("identifier", ev::fromDouble(pt.identifier));
    b.set("target", describeTarget(pt.target));
    b.set("clientX", ev::fromDouble(pt.clientX));
    b.set("clientY", ev::fromDouble(pt.clientY));
    b.set("pageX", ev::fromDouble(pt.pageX));
    b.set("pageY", ev::fromDouble(pt.pageY));
    b.set("screenX", ev::fromDouble(pt.screenX));
    b.set("screenY", ev::fromDouble(pt.screenY));
    b.set("force", ev::fromDouble(pt.force));
    return b.get();
}

Value makeTouchListValue(const std::vector<dom::TouchPoint>& points) {
    Value listObj = g_touchListClass.make(nullptr, [](void*) {});
    ev::Persistent root(listObj);
    for (size_t i = 0; i < points.size(); ++i) {
        Value t = makeTouchValue(points[i]);
        ev::setElement(root.get(), static_cast<uint32_t>(i), t);
    }
    ev::setProperty(root.get(), "length", ev::fromDouble(points.size()));
    return root.get();
}

} // namespace bro::bronze_host
