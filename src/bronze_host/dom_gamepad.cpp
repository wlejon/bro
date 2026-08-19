// Gamepad polling, navigator.getGamepads(), vibrationActuator dual-rumble & trigger-rumble.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "engine/engine.h"
#include "engine/gamepad.h"

#include <string>
#include <vector>

namespace bro::bronze_host {

Value buildGamepadSnapshot(const engine::GamepadState& gp) {
    ObjectBuilder obj;
    obj.set("id", ev::fromUtf8(gp.id));
    obj.set("index", ev::fromDouble(gp.index));
    obj.set("connected", ev::fromBool(gp.connected));
    obj.set("mapping", ev::fromUtf8("standard"));
    obj.set("timestamp", ev::fromDouble(gp.timestampMs));

    Value buttonsArr = hostArrayOf(engine::kGamepadButtonCount, [&gp](size_t i) -> Value {
        float value = gp.buttons[i];
        bool pressed = value >= engine::kGamepadTriggerPressThreshold;
        bool touched = pressed || (value > 0.0f);
        ObjectBuilder b;
        b.set("pressed", ev::fromBool(pressed));
        b.set("touched", ev::fromBool(touched));
        b.set("value", ev::fromDouble(value));
        return b.get();
    });
    obj.set("buttons", buttonsArr);

    Value axesArr = hostArrayOf(engine::kGamepadAxisCount, [&gp](size_t i) -> Value {
        return ev::fromDouble(gp.axes[i]);
    });
    obj.set("axes", axesArr);

    int index = gp.index;
    ObjectBuilder actuator;
    actuator.set("type", ev::fromUtf8("dual-rumble"));
    Value effectsArr = hostArrayOf(2, [](size_t i) -> Value {
        return ev::fromUtf8(i == 0 ? "dual-rumble" : "trigger-rumble");
    });
    actuator.set("effects", effectsArr);

    actuator.def("playEffect", 2, [index](Value, std::span<const Value> a) -> Value {
        auto* engine = hostEngine();
        if (!engine) return ev::throwError("playEffect: no engine");

        bool triggerRumble = false;
        if (a.size() >= 1 && !ev::isObject(a[0]) && !ev::isUndefined(a[0])) {
            std::string t = ev::toUtf8(a[0]);
            triggerRumble = (t == "trigger-rumble");
            if (!triggerRumble && t != "dual-rumble") {
                return ev::throwTypeError(
                    "playEffect: only \"dual-rumble\" and \"trigger-rumble\" are supported");
            }
        }

        double duration = 0.0, strong = 0.0, weak = 0.0;
        double leftTrigger = 0.0, rightTrigger = 0.0;
        if (a.size() >= 2 && ev::isObject(a[1])) {
            Value params = a[1];
            Value v = ev::getProperty(params, "duration");
            if (!ev::isUndefined(v) && !ev::isObject(v)) duration = ev::toDouble(v);
            v = ev::getProperty(params, "strongMagnitude");
            if (!ev::isUndefined(v) && !ev::isObject(v)) strong = ev::toDouble(v);
            v = ev::getProperty(params, "weakMagnitude");
            if (!ev::isUndefined(v) && !ev::isObject(v)) weak = ev::toDouble(v);
            v = ev::getProperty(params, "leftTrigger");
            if (!ev::isUndefined(v) && !ev::isObject(v)) leftTrigger = ev::toDouble(v);
            v = ev::getProperty(params, "rightTrigger");
            if (!ev::isUndefined(v) && !ev::isObject(v)) rightTrigger = ev::toDouble(v);
        }

        bool ok = engine->gamepadRumble(index, static_cast<float>(strong),
                                        static_cast<float>(weak),
                                        static_cast<int>(duration));
        if (triggerRumble) {
            ok = engine->gamepadRumbleTriggers(index,
                                               static_cast<float>(leftTrigger),
                                               static_cast<float>(rightTrigger),
                                               static_cast<int>(duration)) && ok;
        }

        ev::Persistent p{ev::createPromise()};
        ev::resolvePromise(p.get(), ev::fromUtf8(ok ? "complete" : "preempted"));
        return p.get();
    });

    actuator.def("reset", 0, [index](Value, std::span<const Value>) -> Value {
        if (auto* engine = hostEngine()) {
            engine->gamepadRumble(index, 0.0f, 0.0f, 0);
            engine->gamepadRumbleTriggers(index, 0.0f, 0.0f, 0);
        }
        ev::Persistent p{ev::createPromise()};
        ev::resolvePromise(p.get(), ev::fromUtf8("complete"));
        return p.get();
    });

    obj.set("vibrationActuator", actuator.get());
    return obj.get();
}

Value makeNavigatorValue() {
    ObjectBuilder b;
    b.set("userAgent", ev::fromUtf8("Bro/1.0"));
    b.set("platform", ev::fromUtf8("Win32"));
    b.set("language", ev::fromUtf8("en-US"));
    b.set("maxTouchPoints", ev::fromDouble(0));
    b.def("getGamepads", 0, [](Value, std::span<const Value>) {
        auto* engine = hostEngine();
        if (!engine) {
            return hostArrayOf(0, [](size_t) { return ev::null(); });
        }
        const auto& pads = engine->gamepads();
        return hostArrayOf(pads.size(), [&pads](size_t i) -> Value {
            if (!pads[i].connected) return ev::null();
            return buildGamepadSnapshot(pads[i]);
        });
    });
    return b.get();
}

}  // namespace bro::bronze_host
