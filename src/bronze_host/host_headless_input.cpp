#include "bronze_host/host_headless_internal.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "engine/engine.h"
#include "engine/gamepad.h"
#include "engine/window_host.h"
#include <SDL3/SDL.h>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

int scancodeForKeycode(int keycode) {
    if (keycode == 0) return 0;
    return static_cast<int>(SDL_GetScancodeFromKey(
        static_cast<SDL_Keycode>(keycode), nullptr));
}

int gamepadResolveIndex(Value arg, int (*fromName)(const std::string&)) {
    if (ev::isString(arg)) {
        std::string s = ev::toUtf8(arg);
        return fromName(s);
    }
    if (ev::isNumber(arg)) {
        return static_cast<int>(ev::toDouble(arg));
    }
    return -1;
}

} // namespace

void installHeadlessInput(engine::Engine& engine) {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    // Rooted: every registration below allocates (the function, and
    // registerGlobal's define on the live realms).
    const Rooted gObj(gt.found && ev::isObject(gt.value) ? gt.value : Value::fromUndefined());

    auto regBoth = [&](const char* name, Value valIn) {
        const Rooted val(valIn);
        ev::registerGlobal(name, val);
        if (ev::isObject(gObj)) {
            ev::setProperty(gObj, name, val);
        }
    };

    // -----------------------------------------------------------------------
    // Mouse
    // -----------------------------------------------------------------------

    // mouseDown(x, y [, button, windowId])
    regBoth("mouseDown", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 2) return ev::throwTypeError("mouseDown(x, y [, button, windowId]) requires x and y");
            double x = ev::toDouble(a[0]);
            double y = ev::toDouble(a[1]);
            int btn = a.size() > 2 && !ev::isUndefined(a[2]) ? static_cast<int>(ev::toDouble(a[2])) : 0;
            const uint64_t wid = argWindowId(a, 3);
            const float fy = toWindowY(&engine, y, wid);
            if (wid) engine.hostMouseDown(wid, static_cast<float>(x), fy, domToSdlButton(btn));
            else engine.handleMouseDown(static_cast<float>(x), fy, domToSdlButton(btn));
            engine.flush();
            return ev::undefined();
        }, 2, "mouseDown"));

    // mouseUp(x, y [, button, windowId])
    regBoth("mouseUp", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 2) return ev::throwTypeError("mouseUp(x, y [, button, windowId]) requires x and y");
            double x = ev::toDouble(a[0]);
            double y = ev::toDouble(a[1]);
            int btn = a.size() > 2 && !ev::isUndefined(a[2]) ? static_cast<int>(ev::toDouble(a[2])) : 0;
            const uint64_t wid = argWindowId(a, 3);
            const float fy = toWindowY(&engine, y, wid);
            if (wid) engine.hostMouseUp(wid, static_cast<float>(x), fy, domToSdlButton(btn));
            else engine.handleMouseUp(static_cast<float>(x), fy, domToSdlButton(btn));
            engine.flush();
            return ev::undefined();
        }, 2, "mouseUp"));

    // mouseMove(x, y [, windowId])
    regBoth("mouseMove", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 2) return ev::throwTypeError("mouseMove(x, y [, windowId]) requires x and y");
            double x = ev::toDouble(a[0]);
            double y = ev::toDouble(a[1]);
            const uint64_t wid = argWindowId(a, 2);
            float fx = static_cast<float>(x), fy = toWindowY(&engine, y, wid);
            if (wid) {
                auto* h = engine.windowHostById(wid);
                float lx = h ? h->lastMouseX : fx, ly = h ? h->lastMouseY : fy;
                engine.hostMouseMove(wid, fx, fy, fx - lx, fy - ly);
            } else {
                engine.handleMouseMove(fx, fy, fx - engine.getLastMouseX(),
                                       fy - engine.getLastMouseY());
            }
            engine.flush();
            return ev::undefined();
        }, 2, "mouseMove"));

    // currentCursor([windowId])
    regBoth("currentCursor", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            uint64_t wid = argWindowId(a, 0);
            return ev::fromUtf8(engine.resolvedCursor(wid));
        }, 0, "currentCursor"));

    // click(x, y [, button, windowId])
    regBoth("click", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 2) return ev::throwTypeError("click(x, y [, button, windowId]) requires x and y");
            double x = ev::toDouble(a[0]);
            double y = ev::toDouble(a[1]);
            int btn = a.size() > 2 && !ev::isUndefined(a[2]) ? static_cast<int>(ev::toDouble(a[2])) : 0;
            const uint64_t wid = argWindowId(a, 3);
            const float fy = toWindowY(&engine, y, wid);
            int sdlBtn = domToSdlButton(btn);
            if (wid) {
                engine.hostMouseDown(wid, static_cast<float>(x), fy, sdlBtn);
                engine.hostMouseUp(wid, static_cast<float>(x), fy, sdlBtn);
            } else {
                engine.handleMouseDown(static_cast<float>(x), fy, sdlBtn);
                engine.handleMouseUp(static_cast<float>(x), fy, sdlBtn);
            }
            engine.flush();
            return ev::undefined();
        }, 2, "click"));

    // wheel(x, y, dy [, dx, windowId])
    regBoth("wheel", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 3) return ev::throwTypeError("wheel(x, y, dy [, dx, windowId]) requires x, y, dy");
            double x = ev::toDouble(a[0]);
            double y = ev::toDouble(a[1]);
            double dy = ev::toDouble(a[2]);
            double dx = a.size() > 3 && !ev::isUndefined(a[3]) ? ev::toDouble(a[3]) : 0.0;
            const uint64_t wid = argWindowId(a, 4);
            const float fy = toWindowY(&engine, y, wid);
            if (wid) engine.hostWheel(wid, static_cast<float>(x), fy,
                                      static_cast<float>(-dx), static_cast<float>(-dy));
            else engine.handleWheel(static_cast<float>(x), fy,
                                    static_cast<float>(-dx), static_cast<float>(-dy));
            engine.flush();
            return ev::undefined();
        }, 3, "wheel"));

    // -----------------------------------------------------------------------
    // Touch
    // -----------------------------------------------------------------------

    // touchDown(id, x, y [, pressure])
    regBoth("touchDown", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 3) return ev::throwTypeError("touchDown(id, x, y [, pressure]) requires id, x, y");
            int64_t id = static_cast<int64_t>(ev::toDouble(a[0]));
            double x = ev::toDouble(a[1]);
            double y = ev::toDouble(a[2]);
            double pressure = a.size() > 3 && !ev::isUndefined(a[3]) ? ev::toDouble(a[3]) : 1.0;
            engine.handleTouchDown(static_cast<uint64_t>(id), static_cast<float>(x),
                                   toScreenY(&engine, y), static_cast<float>(pressure));
            engine.flush();
            return ev::undefined();
        }, 3, "touchDown"));

    // touchMove(id, x, y [, pressure])
    regBoth("touchMove", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 3) return ev::throwTypeError("touchMove(id, x, y [, pressure]) requires id, x, y");
            int64_t id = static_cast<int64_t>(ev::toDouble(a[0]));
            double x = ev::toDouble(a[1]);
            double y = ev::toDouble(a[2]);
            double pressure = a.size() > 3 && !ev::isUndefined(a[3]) ? ev::toDouble(a[3]) : 1.0;
            engine.handleTouchMove(static_cast<uint64_t>(id), static_cast<float>(x),
                                   toScreenY(&engine, y), static_cast<float>(pressure));
            engine.flush();
            return ev::undefined();
        }, 3, "touchMove"));

    // touchUp(id, x, y)
    regBoth("touchUp", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 3) return ev::throwTypeError("touchUp(id, x, y) requires id, x, y");
            int64_t id = static_cast<int64_t>(ev::toDouble(a[0]));
            double x = ev::toDouble(a[1]);
            double y = ev::toDouble(a[2]);
            engine.handleTouchUp(static_cast<uint64_t>(id), static_cast<float>(x),
                                 toScreenY(&engine, y));
            engine.flush();
            return ev::undefined();
        }, 3, "touchUp"));

    // touchCancel(id, x, y)
    regBoth("touchCancel", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 3) return ev::throwTypeError("touchCancel(id, x, y) requires id, x, y");
            int64_t id = static_cast<int64_t>(ev::toDouble(a[0]));
            double x = ev::toDouble(a[1]);
            double y = ev::toDouble(a[2]);
            engine.handleTouchCancel(static_cast<uint64_t>(id), static_cast<float>(x),
                                     toScreenY(&engine, y));
            engine.flush();
            return ev::undefined();
        }, 3, "touchCancel"));

    // -----------------------------------------------------------------------
    // Keyboard
    // -----------------------------------------------------------------------

    // keyDown(keycode [, scancode, mod, repeat, windowId])
    regBoth("keyDown", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.empty()) return ev::throwTypeError("keyDown(keycode [, scancode, mod, repeat, windowId])");
            int keycode = static_cast<int>(ev::toDouble(a[0]));
            int scancode = a.size() > 1 && !ev::isUndefined(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            int mod = a.size() > 2 && !ev::isUndefined(a[2]) ? static_cast<int>(ev::toDouble(a[2])) : 0;
            bool repeat = a.size() > 3 && !ev::isUndefined(a[3]) ? ev::toBool(a[3]) : false;
            if (scancode == 0) scancode = scancodeForKeycode(keycode);

            const uint64_t wid = argWindowId(a, 4);
            if (wid) engine.hostKeyDown(wid, keycode, scancode, mod, repeat);
            else engine.handleKeyDown(keycode, scancode, mod, repeat);
            engine.flush();
            return ev::undefined();
        }, 1, "keyDown"));

    // keyUp(keycode [, scancode, mod, windowId])
    regBoth("keyUp", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.empty()) return ev::throwTypeError("keyUp(keycode [, scancode, mod, windowId])");
            int keycode = static_cast<int>(ev::toDouble(a[0]));
            int scancode = a.size() > 1 && !ev::isUndefined(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            int mod = a.size() > 2 && !ev::isUndefined(a[2]) ? static_cast<int>(ev::toDouble(a[2])) : 0;
            if (scancode == 0) scancode = scancodeForKeycode(keycode);

            const uint64_t wid = argWindowId(a, 3);
            if (wid) engine.hostKeyUp(wid, keycode, scancode, mod, false);
            else engine.handleKeyUp(keycode, scancode, mod, false);
            engine.flush();
            return ev::undefined();
        }, 1, "keyUp"));

    // textInput(text [, windowId])
    regBoth("textInput", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.empty()) return ev::throwTypeError("textInput(text [, windowId]) requires text");
            std::string text = ev::toUtf8(a[0]);
            const uint64_t wid = argWindowId(a, 1);
            if (wid) engine.hostTextInput(wid, text);
            else engine.handleTextInput(text);
            engine.flush();
            return ev::undefined();
        }, 1, "textInput"));

    // -----------------------------------------------------------------------
    // IME
    // -----------------------------------------------------------------------

    // imeCompose(text [, cursorPos, windowId])
    regBoth("imeCompose", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.empty()) return ev::throwTypeError("imeCompose(text [, cursorPos, windowId]) requires text");
            std::string text = ev::toUtf8(a[0]);
            int cursor = a.size() > 1 && !ev::isUndefined(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : -1;
            const uint64_t wid = argWindowId(a, 2);
            if (wid) engine.hostTextEditing(wid, text, cursor, 0);
            else engine.handleTextEditing(text, cursor, 0);
            engine.flush();
            return ev::undefined();
        }, 1, "imeCompose"));

    // imeCommit(text [, windowId])
    regBoth("imeCommit", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.empty()) return ev::throwTypeError("imeCommit(text [, windowId]) requires text");
            std::string text = ev::toUtf8(a[0]);
            const uint64_t wid = argWindowId(a, 1);
            if (wid) engine.hostTextInput(wid, text);
            else engine.handleTextInput(text);
            engine.flush();
            return ev::undefined();
        }, 1, "imeCommit"));

    // imeCancel([windowId])
    regBoth("imeCancel", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            const uint64_t wid = argWindowId(a, 0);
            if (wid) engine.hostTextEditing(wid, "", 0, 0);
            else engine.handleTextEditing("", 0, 0);
            engine.flush();
            return ev::undefined();
        }, 0, "imeCancel"));

    // -----------------------------------------------------------------------
    // Clipboard
    // -----------------------------------------------------------------------

    // paste(text)
    regBoth("paste", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.empty()) return ev::throwTypeError("paste(text) requires text");
            std::string text = ev::toUtf8(a[0]);
            engine.simulatePaste(text);
            engine.flush();
            return ev::undefined();
        }, 1, "paste"));

    // copy()
    regBoth("copy", ev::makeFunction(
        [&engine](Value, std::span<const Value>) -> Value {
            std::string text = engine.simulateCopy();
            engine.flush();
            return ev::fromUtf8(text);
        }, 0, "copy"));

    // cut()
    regBoth("cut", ev::makeFunction(
        [&engine](Value, std::span<const Value>) -> Value {
            std::string text = engine.simulateCut();
            engine.flush();
            return ev::fromUtf8(text);
        }, 0, "cut"));

    // -----------------------------------------------------------------------
    // Drag & Drop
    // -----------------------------------------------------------------------

    // dropFiles(x, y, paths [, windowId])
    regBoth("dropFiles", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 3) return ev::throwTypeError("dropFiles(x, y, paths [, windowId]) requires x, y, and paths");
            double x = ev::toDouble(a[0]);
            double y = ev::toDouble(a[1]);
            const uint64_t wid = argWindowId(a, 3);
            float fx = static_cast<float>(x), fy = toWindowY(&engine, y, wid);
            if (wid) {
                auto* h = engine.windowHostById(wid);
                float lx = h ? h->lastMouseX : fx, ly = h ? h->lastMouseY : fy;
                engine.hostMouseMove(wid, fx, fy, fx - lx, fy - ly);
            } else {
                engine.handleMouseMove(fx, fy, fx - engine.getLastMouseX(),
                                       fy - engine.getLastMouseY());
            }

            std::vector<std::string> paths;
            Value pathsVal = a[2];
            if (ev::isString(pathsVal)) {
                paths.push_back(ev::toUtf8(pathsVal));
            } else if (ev::isObject(pathsVal)) {
                ev::Persistent p(pathsVal);
                Value lenVal = ev::getProperty(p.get(), "length");
                uint32_t len = static_cast<uint32_t>(ev::toDouble(lenVal));
                for (uint32_t i = 0; i < len; ++i) {
                    paths.push_back(ev::toUtf8(ev::getElement(p.get(), i)));
                }
            }

            if (!paths.empty()) {
                if (wid) engine.hostDropFile(wid, paths, fx, fy);
                else engine.handleDropFile(paths, fx, fy);
            }
            engine.flush();
            return ev::undefined();
        }, 3, "dropFiles"));

    // dropText(x, y, text [, windowId])
    regBoth("dropText", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 3) return ev::throwTypeError("dropText(x, y, text [, windowId]) requires x, y, and text");
            double x = ev::toDouble(a[0]);
            double y = ev::toDouble(a[1]);
            std::string text = ev::toUtf8(a[2]);
            const uint64_t wid = argWindowId(a, 3);
            float fx = static_cast<float>(x), fy = toWindowY(&engine, y, wid);
            if (wid) {
                auto* h = engine.windowHostById(wid);
                float lx = h ? h->lastMouseX : fx, ly = h ? h->lastMouseY : fy;
                engine.hostMouseMove(wid, fx, fy, fx - lx, fy - ly);
                engine.hostDropText(wid, text, fx, fy);
            } else {
                engine.handleMouseMove(fx, fy, fx - engine.getLastMouseX(),
                                       fy - engine.getLastMouseY());
                engine.handleDropText(text, fx, fy);
            }
            engine.flush();
            return ev::undefined();
        }, 3, "dropText"));

    // -----------------------------------------------------------------------
    // Gamepad
    // -----------------------------------------------------------------------

    // gamepadConnect([id])
    regBoth("gamepadConnect", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            std::string id = a.size() > 0 && !ev::isUndefined(a[0]) ? ev::toUtf8(a[0]) : "Virtual Gamepad";
            int idx = engine.gamepadConnectVirtual(id);
            return ev::fromDouble(idx);
        }, 0, "gamepadConnect"));

    // gamepadDisconnect(index)
    regBoth("gamepadDisconnect", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.empty()) return ev::throwTypeError("gamepadDisconnect(index) requires index");
            int idx = static_cast<int>(ev::toDouble(a[0]));
            bool ok = engine.gamepadDisconnectVirtual(idx);
            engine.flush();
            if (!ok) return ev::throwTypeError("gamepadDisconnect: no virtual gamepad at index " + std::to_string(idx));
            return ev::undefined();
        }, 1, "gamepadDisconnect"));

    // gamepadButton(index, button, pressed [, value])
    regBoth("gamepadButton", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 3) return ev::throwTypeError("gamepadButton(index, button, pressed [, value])");
            int idx = static_cast<int>(ev::toDouble(a[0]));
            int button = gamepadResolveIndex(a[1], engine::gamepadButtonIndex);
            if (button < 0 || button >= engine::kGamepadButtonCount)
                return ev::throwTypeError("gamepadButton: unknown button");
            bool pressed = ev::toBool(a[2]);
            float val = a.size() > 3 && !ev::isUndefined(a[3]) ? static_cast<float>(ev::toDouble(a[3])) : (pressed ? 1.0f : 0.0f);
            bool ok = engine.gamepadSetVirtualButton(idx, button, pressed, val);
            engine.flush();
            if (!ok) return ev::throwTypeError("gamepadButton: no virtual gamepad at index " + std::to_string(idx));
            return ev::undefined();
        }, 3, "gamepadButton"));

    // gamepadAxis(index, axis, value)
    regBoth("gamepadAxis", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 3) return ev::throwTypeError("gamepadAxis(index, axis, value)");
            int idx = static_cast<int>(ev::toDouble(a[0]));
            int axis = gamepadResolveIndex(a[1], engine::gamepadAxisIndex);
            if (axis < 0 || axis >= engine::kGamepadAxisCount)
                return ev::throwTypeError("gamepadAxis: unknown axis");
            float val = static_cast<float>(ev::toDouble(a[2]));
            bool ok = engine.gamepadSetVirtualAxis(idx, axis, val);
            engine.flush();
            if (!ok) return ev::throwTypeError("gamepadAxis: no virtual gamepad at index " + std::to_string(idx));
            return ev::undefined();
        }, 3, "gamepadAxis"));
}

} // namespace bro::bronze_host
