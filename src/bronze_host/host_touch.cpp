#include "bronze_host/host_touch.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "dom/element.h"

namespace bro::bronze_host {

HostClass g_touchClass;
HostClass g_touchListClass;
HostClass g_touchEventClass;
HostClass g_gestureEventClass;

void installTouchGlobals() {
    g_touchClass.install("Touch", 1, nullptr, [](ObjectBuilder&) {});

    g_touchListClass.install("TouchList", 0, nullptr, [](ObjectBuilder& b) {
        b.def("item", 1, [](Value self, std::span<const Value> a) -> Value {
            int32_t idx = i32At(a, 0);
            if (idx < 0) return ev::null();
            Value item = ev::getElement(self, static_cast<uint32_t>(idx));
            return ev::isUndefined(item) ? ev::null() : item;
        });
    });

    g_touchEventClass.install("TouchEvent", 1, nullptr, [](ObjectBuilder&) {});
    g_gestureEventClass.install("GestureEvent", 1, nullptr, [](ObjectBuilder&) {});
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
