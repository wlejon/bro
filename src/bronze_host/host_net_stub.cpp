#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "embed/embed.h"

namespace bro::bronze_host {

namespace {
HostClass g_wsStubClass;
}

Value makeBroNetValue() {
    return makeUnavailableNamespace("net", "BRO_WITH_NET");
}

void installNetGlobals() {
    g_wsStubClass.install("WebSocket", 1, [](Value, std::span<const Value>) -> Value {
        return ev::throwError("WebSocket is unavailable: compiled without BRO_WITH_NET");
    }, [](ObjectBuilder& b) {
        b.set("CONNECTING", ev::fromDouble(0));
        b.set("OPEN", ev::fromDouble(1));
        b.set("CLOSING", ev::fromDouble(2));
        b.set("CLOSED", ev::fromDouble(3));
    });
    for (auto [name, v] : {std::pair{"CONNECTING", 0.0}, {"OPEN", 1.0}, {"CLOSING", 2.0}, {"CLOSED", 3.0}}) {
        g_wsStubClass.setStatic(name, ev::fromDouble(v));
    }
}

void drainNetEvents() {
}

void installNetSync(engine::Engine*) {
}

}  // namespace bro::bronze_host
