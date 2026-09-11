#include "bronze_host/host_headless.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"

#include "engine/engine.h"
#include "engine/gamepad.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <string>
#include <vector>
#include <span>

namespace bro::bronze_host {

namespace {

static bool s_hasTestFailure = false;
static std::vector<std::string> s_scriptArgs;

static int domToSdlButton(int domButton) {
    switch (domButton) {
        case 0: return 1;
        case 1: return 2;
        case 2: return 3;
        default: return domButton + 1;
    }
}

static Value makeScriptArgsValue() {
    ev::CallResult parsed = ev::parseJson("[]");
    ev::Persistent arr{parsed.value};
    for (uint32_t i = 0; i < s_scriptArgs.size(); ++i) {
        ev::Persistent s{ev::fromUtf8(s_scriptArgs[i])};
        arr.set(ev::setElement(arr.get(), i, s.get()));
    }
    return arr.get();
}

} // namespace

bool hasTestFailure() {
    return s_hasTestFailure;
}

void clearTestFailure() {
    s_hasTestFailure = false;
}

void setTestFailure(bool failed) {
    s_hasTestFailure = failed;
}

void setScriptArgs(const std::vector<std::string>& args) {
    s_scriptArgs = args;
    if (isWebHostGlobalsInstalled()) {
        ev::registerGlobal("scriptArgs", makeScriptArgsValue());
    }
}

void installHeadlessGlobals(engine::Engine& engine) {
    // 1. advanceTime(double ms)
    ev::registerGlobal("advanceTime", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            double ms = a.empty() ? 0.0 : ev::toDouble(a[0]);
            engine.advanceTime(ms);
            return ev::undefined();
        }, 1, "advanceTime"));

    // 2. flush()
    ev::registerGlobal("flush", ev::makeFunction(
        [&engine](Value, std::span<const Value>) -> Value {
            engine.flush();
            return ev::undefined();
        }, 0, "flush"));

    // 3. sleep(double ms)
    ev::registerGlobal("sleep", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            double ms = a.empty() ? 0.0 : ev::toDouble(a[0]);
            engine.advanceTime(ms);
            return ev::undefined();
        }, 1, "sleep"));

    // 4. screenshot(const std::string& path)
    ev::registerGlobal("screenshot", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            std::string path = a.empty() ? "" : ev::toUtf8(a[0]);
            bool ok = engine.screenshot(path);
            return ev::fromBool(ok);
        }, 1, "screenshot"));

    // 5. assert(bool condition, const std::string& message)
    ev::registerGlobal("assert", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            bool cond = !a.empty() && ev::toBool(a[0]);
            std::string msg = a.size() > 1 ? ev::toUtf8(a[1]) : "assertion failed";
            if (!cond) {
                LOG_ERROR("ASSERTION FAILED: %s", msg.c_str());
                setTestFailure(true);
                engine.setTestFailure(true);
                return ev::fromBool(false);
            }
            return ev::fromBool(true);
        }, 2, "assert"));

    // 6. click(float x, float y)
    ev::registerGlobal("click", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            float x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            int btn = a.size() > 2 ? static_cast<int>(ev::toDouble(a[2])) : 0;
            int sdlBtn = domToSdlButton(btn);
            engine.handleMouseDown(x, y, sdlBtn);
            engine.handleMouseUp(x, y, sdlBtn);
            engine.flush();
            return ev::undefined();
        }, 2, "click"));

    // 7. mouseDown(float x, float y, int btn=0)
    ev::registerGlobal("mouseDown", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            float x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            int btn = a.size() > 2 ? static_cast<int>(ev::toDouble(a[2])) : 0;
            engine.handleMouseDown(x, y, domToSdlButton(btn));
            engine.flush();
            return ev::undefined();
        }, 2, "mouseDown"));

    // 8. mouseUp(float x, float y, int btn=0)
    ev::registerGlobal("mouseUp", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            float x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            int btn = a.size() > 2 ? static_cast<int>(ev::toDouble(a[2])) : 0;
            engine.handleMouseUp(x, y, domToSdlButton(btn));
            engine.flush();
            return ev::undefined();
        }, 2, "mouseUp"));

    // 9. mouseMove(float x, float y)
    ev::registerGlobal("mouseMove", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            float x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            engine.handleMouseMove(x, y, x - engine.getLastMouseX(), y - engine.getLastMouseY());
            engine.flush();
            return ev::undefined();
        }, 2, "mouseMove"));

    // 10. wheel(float x, float y, float dy, float dx=0)
    ev::registerGlobal("wheel", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            float x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            float dy = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
            float dx = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
            engine.handleWheel(x, y, -dx, -dy);
            engine.flush();
            return ev::undefined();
        }, 3, "wheel"));

    // 11. keyDown(int key, int scancode, int mod)
    ev::registerGlobal("keyDown", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            int key = a.size() > 0 ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int scancode = a.size() > 1 ? static_cast<int>(ev::toDouble(a[1])) : 0;
            int mod = a.size() > 2 ? static_cast<int>(ev::toDouble(a[2])) : 0;
            if (scancode == 0 && key != 0) {
                scancode = static_cast<int>(SDL_GetScancodeFromKey(static_cast<SDL_Keycode>(key), nullptr));
            }
            engine.handleKeyDown(key, scancode, mod, false);
            engine.flush();
            return ev::undefined();
        }, 1, "keyDown"));

    // 12. keyUp(int key, int scancode, int mod)
    ev::registerGlobal("keyUp", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            int key = a.size() > 0 ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int scancode = a.size() > 1 ? static_cast<int>(ev::toDouble(a[1])) : 0;
            int mod = a.size() > 2 ? static_cast<int>(ev::toDouble(a[2])) : 0;
            if (scancode == 0 && key != 0) {
                scancode = static_cast<int>(SDL_GetScancodeFromKey(static_cast<SDL_Keycode>(key), nullptr));
            }
            engine.handleKeyUp(key, scancode, mod, false);
            engine.flush();
            return ev::undefined();
        }, 1, "keyUp"));

    // 13. textInput(const std::string& text)
    ev::registerGlobal("textInput", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            std::string text = a.size() > 0 ? ev::toUtf8(a[0]) : "";
            engine.handleTextInput(text);
            engine.flush();
            return ev::undefined();
        }, 1, "textInput"));

    // 14. scriptArgs
    ev::registerGlobal("scriptArgs", makeScriptArgsValue());

    // 15. gamepadConnect(id)
    ev::registerGlobal("gamepadConnect", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            std::string id = a.size() > 0 ? ev::toUtf8(a[0]) : "Virtual Gamepad";
            int idx = engine.gamepadConnectVirtual(id);
            return ev::fromDouble(idx);
        }, 1, "gamepadConnect"));

    // 16. gamepadDisconnect(index)
    ev::registerGlobal("gamepadDisconnect", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            int idx = a.size() > 0 ? static_cast<int>(ev::toDouble(a[0])) : 0;
            bool ok = engine.gamepadDisconnectVirtual(idx);
            return ev::fromBool(ok);
        }, 1, "gamepadDisconnect"));

    // 17. gamepadButton(index, button, pressed, value)
    ev::registerGlobal("gamepadButton", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            int idx = a.size() > 0 ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int buttonIdx = 0;
            if (a.size() > 1) {
                if (ev::isNumber(a[1])) {
                    buttonIdx = static_cast<int>(ev::toDouble(a[1]));
                } else {
                    std::string bname = ev::toUtf8(a[1]);
                    buttonIdx = engine::gamepadButtonIndex(bname);
                }
            }
            bool pressed = a.size() > 2 ? ev::toBool(a[2]) : false;
            float val = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : (pressed ? 1.0f : 0.0f);
            bool ok = engine.gamepadSetVirtualButton(idx, buttonIdx, pressed, val);
            return ev::fromBool(ok);
        }, 4, "gamepadButton"));

    // 18. gamepadAxis(index, axis, value)
    ev::registerGlobal("gamepadAxis", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            int idx = a.size() > 0 ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int axisIdx = 0;
            if (a.size() > 1) {
                if (ev::isNumber(a[1])) {
                    axisIdx = static_cast<int>(ev::toDouble(a[1]));
                } else {
                    std::string aname = ev::toUtf8(a[1]);
                    axisIdx = engine::gamepadAxisIndex(aname);
                }
            }
            float val = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
            bool ok = engine.gamepadSetVirtualAxis(idx, axisIdx, val);
            return ev::fromBool(ok);
        }, 3, "gamepadAxis"));
}

} // namespace bro::bronze_host
