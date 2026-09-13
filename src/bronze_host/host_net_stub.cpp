#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "embed/embed.h"

#include <api/api.h>

namespace bro::bronze_host {

Value makeBroNetValue() {
    return makeUnavailableNamespace("net", "BRO_WITH_NET");
}

void installNetGlobals() {
    brokit::api::installWebSocket();
    brokit::api::installWebSocketJS();
    auto wsVal = ev::globalValue("WebSocket");
    if (wsVal.found && ev::isObject(wsVal.value)) {
        auto proto = ev::getProperty(wsVal.value, "prototype");
        if (ev::isObject(proto)) {
            for (auto [name, v] : {std::pair{"CONNECTING", 0.0}, {"OPEN", 1.0}, {"CLOSING", 2.0}, {"CLOSED", 3.0}}) {
                ev::setProperty(proto, name, ev::fromDouble(v));
                ev::setProperty(wsVal.value, name, ev::fromDouble(v));
            }
        }
    }
}

void drainNetEvents() {
    auto netTick = ev::globalValue("__brokit_net_tick");
    if (netTick.found && ev::isFunction(netTick.value)) {
        ev::call(netTick.value, ev::undefined(), {});
    }
    auto wsTick = ev::globalValue("__brokit_ws_tick");
    if (wsTick.found && ev::isFunction(wsTick.value)) {
        ev::call(wsTick.value, ev::undefined(), {});
    }
}

void installNetSync(engine::Engine*) {
}

}  // namespace bro::bronze_host
