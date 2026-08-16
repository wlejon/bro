#include "js/event_dispatch_internal.h"
#include "js/dom_bindings.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/shadow_root.h"
#include "dom/event.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace bro::js {

// Legacy `keyCode` for a KeyboardEvent, derived from its `code`/`key`.
//
// Deprecated for a decade and still what a great deal of shipped code reads:
// CodeMirror keys its entire binding table off `e.keyCode`, and so does most
// library code written before `key` existed. Reporting a constant 0 makes every
// one of those bindings resolve to the same non-key — no Enter, no Backspace,
// no arrows, and no way for the page to tell which letter was pressed.
//
// The values are the "legacy key code" set the browsers agreed on (Windows
// virtual-key codes for the most part), so a US layout is the reference. `code`
// is physical, which is exactly the right input for it; `key` only answers for
// the printable characters `code` cannot name.
static int legacyKeyCodeFor(const std::string& code, const std::string& key) {
    // KeyA..KeyZ → 65..90, Digit0..Digit9 → 48..57, Numpad0..9 → 96..105.
    if (code.size() == 4 && code.compare(0, 3, "Key") == 0 &&
        code[3] >= 'A' && code[3] <= 'Z')
        return code[3];
    if (code.size() == 6 && code.compare(0, 5, "Digit") == 0 &&
        code[5] >= '0' && code[5] <= '9')
        return code[5];
    if (code.size() == 7 && code.compare(0, 6, "Numpad") == 0 &&
        code[6] >= '0' && code[6] <= '9')
        return 96 + (code[6] - '0');
    // F1..F24 → 112..135.
    if (code.size() >= 2 && code[0] == 'F' && code[1] >= '0' && code[1] <= '9') {
        int n = std::atoi(code.c_str() + 1);
        if (n >= 1 && n <= 24) return 111 + n;
    }

    static const struct { const char* code; int keyCode; } kMap[] = {
        {"Backspace", 8},   {"Tab", 9},          {"Enter", 13},
        {"ShiftLeft", 16},  {"ShiftRight", 16},  {"ControlLeft", 17},
        {"ControlRight", 17}, {"AltLeft", 18},   {"AltRight", 18},
        {"Pause", 19},      {"CapsLock", 20},    {"Escape", 27},
        {"Space", 32},      {"PageUp", 33},      {"PageDown", 34},
        {"End", 35},        {"Home", 36},        {"ArrowLeft", 37},
        {"ArrowUp", 38},    {"ArrowRight", 39},  {"ArrowDown", 40},
        {"PrintScreen", 44},{"Insert", 45},      {"Delete", 46},
        {"MetaLeft", 91},   {"MetaRight", 92},   {"ContextMenu", 93},
        {"NumpadMultiply", 106}, {"NumpadAdd", 107}, {"NumpadSubtract", 109},
        {"NumpadDecimal", 110},  {"NumpadDivide", 111}, {"NumpadEnter", 13},
        {"NumLock", 144},   {"ScrollLock", 145},
        {"Semicolon", 186}, {"Equal", 187},      {"Comma", 188},
        {"Minus", 189},     {"Period", 190},     {"Slash", 191},
        {"Backquote", 192}, {"BracketLeft", 219},{"Backslash", 220},
        {"BracketRight", 221}, {"Quote", 222},
    };
    for (const auto& e : kMap)
        if (code == e.code) return e.keyCode;

    // No `code` (a synthesized event, or a key the platform did not name):
    // fall back to the character itself, uppercased, which is what the legacy
    // table holds for the letter and digit rows.
    if (key.size() == 1) {
        unsigned char c = static_cast<unsigned char>(key[0]);
        if (c >= 'a' && c <= 'z') return c - 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
        if (c == ' ') return 32;
    }
    if (key == "Enter") return 13;
    if (key == "Tab") return 9;
    if (key == "Escape") return 27;
    if (key == "Backspace") return 8;
    return 0;
}

// Build a short "click on #my-id" / "click on div.foo" / "click on div"
// description used in JS error log lines for listener invocations.
std::string describeListener(const std::string& evtType,
                             const bro::dom::Element* el) {
    std::string out = evtType + " on ";
    if (!el) { out += "(detached)"; return out; }
    std::string id = el->getAttribute("id");
    if (!id.empty()) { out += "#"; out += id; return out; }
    out += el->tagName();
    std::string cls = el->getAttribute("class");
    if (!cls.empty()) {
        out += ".";
        // First class only — keeps the label short.
        size_t sp = cls.find_first_of(" \t");
        out += (sp == std::string::npos) ? cls : cls.substr(0, sp);
    }
    return out;
}

// C-function methods for plain JS event objects. They set flag properties
// on the JS object which are read back after each listener call and
// propagated to the C++ Event.
static JSValue js_ev_stopPropagation(JSContext* ctx, JSValueConst this_val,
                                     int /*argc*/, JSValueConst* /*argv*/) {
    JS_SetPropertyStr(ctx, this_val, "_stopped", JS_TRUE);
    return JS_UNDEFINED;
}

static JSValue js_ev_preventDefault(JSContext* ctx, JSValueConst this_val,
                                    int /*argc*/, JSValueConst* /*argv*/) {
    JS_SetPropertyStr(ctx, this_val, "_prevented", JS_TRUE);
    JS_SetPropertyStr(ctx, this_val, "defaultPrevented", JS_TRUE);
    return JS_UNDEFINED;
}

static JSValue js_ev_stopImmediatePropagation(JSContext* ctx, JSValueConst this_val,
                                              int /*argc*/, JSValueConst* /*argv*/) {
    JS_SetPropertyStr(ctx, this_val, "_stopped", JS_TRUE);
    JS_SetPropertyStr(ctx, this_val, "_immediateStopped", JS_TRUE);
    return JS_UNDEFINED;
}

std::vector<EventPathEntry> buildEventPath(bro::dom::Element* target) {
    std::vector<EventPathEntry> path;

    // Current target as we walk up
    bro::dom::Element* current = target;
    bro::dom::Element* effectiveTarget = target;

    while (current) {
        path.push_back({current, effectiveTarget});

        // Check if current element's parent is a ShadowRoot
        auto* parentNode = current->parentNode();
        if (parentNode && parentNode->nodeType() == bro::dom::NodeType::DocumentFragment) {
            auto* sr = dynamic_cast<bro::dom::ShadowRoot*>(parentNode);
            if (sr && sr->host()) {
                // Crossing shadow boundary: retarget to the host
                effectiveTarget = sr->host();
                current = sr->host();
                continue;
            }
        }

        // Normal parent traversal
        current = current->parentElement();
    }

    return path;
}

// composedPath() implementation — returns JS array of elements in the event path
static JSValue js_ev_composedPath(JSContext* ctx, JSValueConst this_val,
                                  int /*argc*/, JSValueConst* /*argv*/) {
    // Retrieve the stashed path array
    JSValue pathArr = JS_GetPropertyStr(ctx, this_val, "_composedPath");
    if (!JS_IsUndefined(pathArr) && !JS_IsNull(pathArr)) {
        return pathArr; // already owns a ref from GetProperty
    }
    JS_FreeValue(ctx, pathArr);
    return JS_NewArray(ctx); // empty array fallback
}

void populateJsEvent(JSContext* ctx, JSValue jsEvent, bro::dom::Event& event) {
    JS_SetPropertyStr(ctx, jsEvent, "type",
                      JS_NewString(ctx, event.type().c_str()));
    JS_SetPropertyStr(ctx, jsEvent, "timeStamp",
                      JS_NewFloat64(ctx, event.timeStamp()));
    JS_SetPropertyStr(ctx, jsEvent, "bubbles",
                      JS_NewBool(ctx, event.bubbles()));
    JS_SetPropertyStr(ctx, jsEvent, "cancelable",
                      JS_NewBool(ctx, event.cancelable()));
    JS_SetPropertyStr(ctx, jsEvent, "composed",
                      JS_NewBool(ctx, event.composed()));
    JS_SetPropertyStr(ctx, jsEvent, "isTrusted",
                      JS_NewBool(ctx, event.isTrusted()));
    JS_SetPropertyStr(ctx, jsEvent, "eventPhase",
                      JS_NewInt32(ctx, NONE));
    JS_SetPropertyStr(ctx, jsEvent, "defaultPrevented",
                      JS_NewBool(ctx, false));

    // CustomEvent.detail — the string payload a native dispatcher put on the
    // event. Only reached when dispatch had to BUILD this JS object, i.e. the
    // event came from C++; a JS-originated CustomEvent arrives as its own
    // object (originalJsEvent) with the caller's real detail already on it,
    // whatever type that is.
    auto* customEvt = dynamic_cast<bro::dom::CustomEvent*>(&event);
    if (customEvt) {
        JS_SetPropertyStr(ctx, jsEvent, "detail",
                          JS_NewString(ctx, customEvt->detail().c_str()));
    }

    // MouseEvent properties
    auto* mouseEvt = dynamic_cast<bro::dom::MouseEvent*>(&event);
    if (mouseEvt) {
        JS_SetPropertyStr(ctx, jsEvent, "clientX",
                          JS_NewFloat64(ctx, mouseEvt->clientX()));
        JS_SetPropertyStr(ctx, jsEvent, "clientY",
                          JS_NewFloat64(ctx, mouseEvt->clientY()));
        JS_SetPropertyStr(ctx, jsEvent, "pageX",
                          JS_NewFloat64(ctx, mouseEvt->pageX()));
        JS_SetPropertyStr(ctx, jsEvent, "pageY",
                          JS_NewFloat64(ctx, mouseEvt->pageY()));
        JS_SetPropertyStr(ctx, jsEvent, "screenX",
                          JS_NewFloat64(ctx, mouseEvt->screenX()));
        JS_SetPropertyStr(ctx, jsEvent, "screenY",
                          JS_NewFloat64(ctx, mouseEvt->screenY()));
        JS_SetPropertyStr(ctx, jsEvent, "offsetX",
                          JS_NewFloat64(ctx, mouseEvt->offsetX()));
        JS_SetPropertyStr(ctx, jsEvent, "offsetY",
                          JS_NewFloat64(ctx, mouseEvt->offsetY()));
        JS_SetPropertyStr(ctx, jsEvent, "movementX",
                          JS_NewFloat64(ctx, mouseEvt->movementX()));
        JS_SetPropertyStr(ctx, jsEvent, "movementY",
                          JS_NewFloat64(ctx, mouseEvt->movementY()));
        JS_SetPropertyStr(ctx, jsEvent, "button",
                          JS_NewInt32(ctx, mouseEvt->button()));
        JS_SetPropertyStr(ctx, jsEvent, "buttons",
                          JS_NewInt32(ctx, mouseEvt->buttons()));
        // Legacy `which`: button + 1 (1 left, 2 middle, 3 right), 0 when no
        // button is involved. Still the first thing a lot of library code
        // reads — CodeMirror decides a press is a left click with
        // `e.which == 1` and ignores it entirely otherwise, so a missing
        // `which` reads as "some button I don't handle" and the editor never
        // takes focus or moves its cursor.
        JS_SetPropertyStr(ctx, jsEvent, "which",
                          JS_NewInt32(ctx, mouseEvt->button() >= 0
                                               ? mouseEvt->button() + 1 : 0));
        JS_SetPropertyStr(ctx, jsEvent, "detail",
                          JS_NewInt32(ctx, mouseEvt->detail()));
        JS_SetPropertyStr(ctx, jsEvent, "ctrlKey",
                          JS_NewBool(ctx, mouseEvt->ctrlKey()));
        JS_SetPropertyStr(ctx, jsEvent, "shiftKey",
                          JS_NewBool(ctx, mouseEvt->shiftKey()));
        JS_SetPropertyStr(ctx, jsEvent, "altKey",
                          JS_NewBool(ctx, mouseEvt->altKey()));
        JS_SetPropertyStr(ctx, jsEvent, "metaKey",
                          JS_NewBool(ctx, mouseEvt->metaKey()));
        if (mouseEvt->relatedTarget()) {
            JSValue global = JS_GetGlobalObject(ctx);
            JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
            if (!JS_IsUndefined(elemMap)) {
                std::string key = std::to_string(mouseEvt->relatedTarget()->nodeId());
                JSValue rtElem = JS_GetPropertyStr(ctx, elemMap, key.c_str());
                JS_SetPropertyStr(ctx, jsEvent, "relatedTarget", rtElem);
            } else {
                JS_SetPropertyStr(ctx, jsEvent, "relatedTarget", JS_NULL);
            }
            JS_FreeValue(ctx, elemMap);
            JS_FreeValue(ctx, global);
        } else {
            JS_SetPropertyStr(ctx, jsEvent, "relatedTarget", JS_NULL);
        }

        // PointerEvent properties. Pointer events are synthesized from mouse
        // input (Engine::dispatchPointerAlias — pointerId 1, "mouse") and from
        // touch contacts (Engine::handleTouch* — per-contact ids ≥ 2, "touch");
        // the MouseEvent carries the per-pointer payload. Handlers are
        // duck-typed on MouseEvent fields (clientX/button/…), already set above;
        // these add the pointer-only fields so e.pointerId etc. are defined.
        if (event.type().rfind("pointer", 0) == 0) {
            JS_SetPropertyStr(ctx, jsEvent, "pointerId",
                              JS_NewInt32(ctx, mouseEvt->pointerId()));
            JS_SetPropertyStr(ctx, jsEvent, "pointerType",
                              JS_NewString(ctx, mouseEvt->pointerType().c_str()));
            JS_SetPropertyStr(ctx, jsEvent, "isPrimary",
                              JS_NewBool(ctx, mouseEvt->isPrimaryPointer()));
            JS_SetPropertyStr(ctx, jsEvent, "width", JS_NewFloat64(ctx, 1));
            JS_SetPropertyStr(ctx, jsEvent, "height", JS_NewFloat64(ctx, 1));
            double pressure = mouseEvt->pressure();
            if (pressure < 0.0) pressure = mouseEvt->buttons() != 0 ? 0.5 : 0.0;
            JS_SetPropertyStr(ctx, jsEvent, "pressure", JS_NewFloat64(ctx, pressure));
            JS_SetPropertyStr(ctx, jsEvent, "tangentialPressure", JS_NewFloat64(ctx, 0));
            JS_SetPropertyStr(ctx, jsEvent, "tiltX", JS_NewFloat64(ctx, 0));
            JS_SetPropertyStr(ctx, jsEvent, "tiltY", JS_NewFloat64(ctx, 0));
            JS_SetPropertyStr(ctx, jsEvent, "twist", JS_NewFloat64(ctx, 0));
        }

        // WheelEvent properties
        auto* wheelEvt = dynamic_cast<bro::dom::WheelEvent*>(&event);
        if (wheelEvt) {
            JS_SetPropertyStr(ctx, jsEvent, "deltaX",
                              JS_NewFloat64(ctx, wheelEvt->deltaX()));
            JS_SetPropertyStr(ctx, jsEvent, "deltaY",
                              JS_NewFloat64(ctx, wheelEvt->deltaY()));
            JS_SetPropertyStr(ctx, jsEvent, "deltaZ",
                              JS_NewFloat64(ctx, wheelEvt->deltaZ()));
            JS_SetPropertyStr(ctx, jsEvent, "deltaMode",
                              JS_NewInt32(ctx, wheelEvt->deltaMode()));
        }
    }

    // KeyboardEvent properties
    auto* keyEvt = dynamic_cast<bro::dom::KeyboardEvent*>(&event);
    if (keyEvt) {
        JS_SetPropertyStr(ctx, jsEvent, "key",
                          JS_NewString(ctx, keyEvt->key().c_str()));
        JS_SetPropertyStr(ctx, jsEvent, "code",
                          JS_NewString(ctx, keyEvt->code().c_str()));
        JS_SetPropertyStr(ctx, jsEvent, "ctrlKey",
                          JS_NewBool(ctx, keyEvt->ctrlKey()));
        JS_SetPropertyStr(ctx, jsEvent, "shiftKey",
                          JS_NewBool(ctx, keyEvt->shiftKey()));
        JS_SetPropertyStr(ctx, jsEvent, "altKey",
                          JS_NewBool(ctx, keyEvt->altKey()));
        JS_SetPropertyStr(ctx, jsEvent, "metaKey",
                          JS_NewBool(ctx, keyEvt->metaKey()));
        JS_SetPropertyStr(ctx, jsEvent, "repeat",
                          JS_NewBool(ctx, keyEvt->repeat()));
        JS_SetPropertyStr(ctx, jsEvent, "isComposing",
                          JS_NewBool(ctx, keyEvt->isComposing()));
        JS_SetPropertyStr(ctx, jsEvent, "location",
                          JS_NewInt32(ctx, keyEvt->location()));
        // Legacy properties (deprecated but widely used). keyCode and which
        // carry the same value on keydown/keyup; charCode belongs to keypress,
        // which bro does not fire, so it stays 0.
        const int legacyCode = legacyKeyCodeFor(keyEvt->code(), keyEvt->key());
        JS_SetPropertyStr(ctx, jsEvent, "keyCode",
                          JS_NewInt32(ctx, legacyCode));
        JS_SetPropertyStr(ctx, jsEvent, "charCode",
                          JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, jsEvent, "which",
                          JS_NewInt32(ctx, legacyCode));
    }

    // FocusEvent properties
    auto* focusEvt = dynamic_cast<bro::dom::FocusEvent*>(&event);
    if (focusEvt) {
        if (focusEvt->relatedTarget()) {
            JSValue global = JS_GetGlobalObject(ctx);
            JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
            if (!JS_IsUndefined(elemMap)) {
                std::string key = std::to_string(focusEvt->relatedTarget()->nodeId());
                JSValue rtElem = JS_GetPropertyStr(ctx, elemMap, key.c_str());
                JS_SetPropertyStr(ctx, jsEvent, "relatedTarget", rtElem);
            } else {
                JS_SetPropertyStr(ctx, jsEvent, "relatedTarget", JS_NULL);
            }
            JS_FreeValue(ctx, elemMap);
            JS_FreeValue(ctx, global);
        } else {
            JS_SetPropertyStr(ctx, jsEvent, "relatedTarget", JS_NULL);
        }
    }

    // InputEvent properties
    auto* inputEvt = dynamic_cast<bro::dom::InputEvent*>(&event);
    if (inputEvt) {
        if (inputEvt->data().empty()) {
            JS_SetPropertyStr(ctx, jsEvent, "data", JS_NULL);
        } else {
            JS_SetPropertyStr(ctx, jsEvent, "data",
                              JS_NewString(ctx, inputEvt->data().c_str()));
        }
        JS_SetPropertyStr(ctx, jsEvent, "inputType",
                          JS_NewString(ctx, inputEvt->inputType().c_str()));
        JS_SetPropertyStr(ctx, jsEvent, "isComposing",
                          JS_NewBool(ctx, inputEvt->isComposing()));
    }

    // CompositionEvent properties. Unlike InputEvent.data, an empty string
    // stays "" (compositionend reports "" on cancel, not null).
    auto* compEvt = dynamic_cast<bro::dom::CompositionEvent*>(&event);
    if (compEvt) {
        JS_SetPropertyStr(ctx, jsEvent, "data",
                          JS_NewString(ctx, compEvt->data().c_str()));
    }

    // SubmitEvent — carries the submit button (if any) that triggered it.
    auto* submitEvt = dynamic_cast<bro::dom::SubmitEvent*>(&event);
    if (submitEvt) {
        auto* sub = submitEvt->submitter();
        JS_SetPropertyStr(ctx, jsEvent, "submitter",
                          sub ? DomBindings::wrapElement(ctx, sub) : JS_NULL);
    }

    // TransitionEvent properties
    auto* transEvt = dynamic_cast<bro::dom::TransitionEvent*>(&event);
    if (transEvt) {
        JS_SetPropertyStr(ctx, jsEvent, "propertyName",
                          JS_NewString(ctx, transEvt->propertyName().c_str()));
        JS_SetPropertyStr(ctx, jsEvent, "elapsedTime",
                          JS_NewFloat64(ctx, transEvt->elapsedTime()));
        JS_SetPropertyStr(ctx, jsEvent, "pseudoElement",
                          JS_NewString(ctx, transEvt->pseudoElement().c_str()));
    }

    // AnimationEvent properties
    auto* animEvt = dynamic_cast<bro::dom::AnimationEvent*>(&event);
    if (animEvt) {
        JS_SetPropertyStr(ctx, jsEvent, "animationName",
                          JS_NewString(ctx, animEvt->animationName().c_str()));
        JS_SetPropertyStr(ctx, jsEvent, "elapsedTime",
                          JS_NewFloat64(ctx, animEvt->elapsedTime()));
        JS_SetPropertyStr(ctx, jsEvent, "pseudoElement",
                          JS_NewString(ctx, animEvt->pseudoElement().c_str()));
    }

    // ClipboardEvent — clipboardData shaped like the web's DataTransfer:
    //   .getData("text/plain")         existing text path
    //   .setData(type, data)           existing write-back path
    //   .items[i]                      {kind, type, getAsFile(), getAsString(cb)}
    //   .files[i]                      File objects (from brokit's Blob/File) for binary items
    auto* clipEvt = dynamic_cast<bro::dom::ClipboardEvent*>(&event);
    if (clipEvt) {
        JSValue dt = JS_NewObject(ctx);
        std::string text = clipEvt->clipboardText();
        JS_SetPropertyStr(ctx, dt, "_text", JS_NewString(ctx, text.c_str()));
        JS_SetPropertyStr(ctx, dt, "getData", JS_NewCFunction2(ctx,
            [](JSContext* c, JSValueConst this_val, int, JSValueConst*) -> JSValue {
                return JS_GetPropertyStr(c, this_val, "_text");
            }, "getData", 1, JS_CFUNC_generic, 0));
        JS_SetPropertyStr(ctx, dt, "setData", JS_NewCFunction2(ctx,
            [](JSContext* c, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                if (argc >= 2) {
                    JS_SetPropertyStr(c, this_val, "_text", JS_DupValue(c, argv[1]));
                }
                return JS_UNDEFINED;
            }, "setData", 2, JS_CFUNC_generic, 0));

        // Build items[] and files[]. File objects come from brokit's globalThis.File
        // so they carry real byte buffers with arrayBuffer()/text()/slice().
        JSValue itemsArr = JS_NewArray(ctx);
        JSValue filesArr = JS_NewArray(ctx);
        uint32_t itemIdx = 0, fileIdx = 0;

        JSValue global = JS_GetGlobalObject(ctx);
        JSValue fileCtor = JS_GetPropertyStr(ctx, global, "File");
        JSValue u8Ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
        bool haveFile = JS_IsConstructor(ctx, fileCtor) && JS_IsConstructor(ctx, u8Ctor);

        for (const auto& it : clipEvt->items()) {
            JSValue itemObj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, itemObj, "type",
                              JS_NewString(ctx, it.mime.c_str()));

            if (!it.bytes.empty() && haveFile) {
                JS_SetPropertyStr(ctx, itemObj, "kind", JS_NewString(ctx, "file"));

                // new Uint8Array(new ArrayBuffer(<bytes>))
                JSValue ab = JS_NewArrayBufferCopy(ctx, it.bytes.data(), it.bytes.size());
                JSValue u8 = JS_CallConstructor(ctx, u8Ctor, 1, &ab);
                JS_FreeValue(ctx, ab);

                // new File([u8], name, {type})
                JSValue parts = JS_NewArray(ctx);
                JS_SetPropertyUint32(ctx, parts, 0, u8);  // transfers u8
                JSValue opts = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, opts, "type", JS_NewString(ctx, it.mime.c_str()));

                std::string name = "clipboard";
                if (it.mime == "image/png") name += ".png";
                else if (it.mime == "image/bmp") name += ".bmp";
                else if (it.mime == "image/jpeg") name += ".jpg";
                JSValue nameVal = JS_NewString(ctx, name.c_str());

                JSValueConst fileArgs[3] = {parts, nameVal, opts};
                JSValue file = JS_CallConstructor(ctx, fileCtor, 3, fileArgs);
                JS_FreeValue(ctx, parts);
                JS_FreeValue(ctx, nameVal);
                JS_FreeValue(ctx, opts);

                // Stash on item for getAsFile(); also push into files[].
                JS_SetPropertyStr(ctx, itemObj, "_file", JS_DupValue(ctx, file));
                JS_SetPropertyUint32(ctx, filesArr, fileIdx++, file);

                JS_SetPropertyStr(ctx, itemObj, "getAsFile", JS_NewCFunction(ctx,
                    [](JSContext* c, JSValueConst this_val, int, JSValueConst*) -> JSValue {
                        return JS_GetPropertyStr(c, this_val, "_file");
                    }, "getAsFile", 0));
                JS_SetPropertyStr(ctx, itemObj, "getAsString", JS_NewCFunction(ctx,
                    [](JSContext*, JSValueConst, int, JSValueConst*) -> JSValue {
                        return JS_UNDEFINED;  // spec: no-op for file items
                    }, "getAsString", 1));
            } else {
                JS_SetPropertyStr(ctx, itemObj, "kind", JS_NewString(ctx, "string"));
                JS_SetPropertyStr(ctx, itemObj, "_text",
                                  JS_NewString(ctx, it.text.c_str()));
                JS_SetPropertyStr(ctx, itemObj, "getAsString", JS_NewCFunction(ctx,
                    [](JSContext* c, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                        if (argc >= 1 && JS_IsFunction(c, argv[0])) {
                            JSValue t = JS_GetPropertyStr(c, this_val, "_text");
                            JSValue r = JS_Call(c, argv[0], JS_UNDEFINED, 1, &t);
                            JS_FreeValue(c, r);
                            JS_FreeValue(c, t);
                        }
                        return JS_UNDEFINED;
                    }, "getAsString", 1));
                JS_SetPropertyStr(ctx, itemObj, "getAsFile", JS_NewCFunction(ctx,
                    [](JSContext*, JSValueConst, int, JSValueConst*) -> JSValue {
                        return JS_NULL;
                    }, "getAsFile", 0));
            }
            JS_SetPropertyUint32(ctx, itemsArr, itemIdx++, itemObj);
        }

        JS_FreeValue(ctx, fileCtor);
        JS_FreeValue(ctx, u8Ctor);
        JS_FreeValue(ctx, global);

        JS_SetPropertyStr(ctx, dt, "items", itemsArr);
        JS_SetPropertyStr(ctx, dt, "files", filesArr);
        JS_SetPropertyStr(ctx, jsEvent, "clipboardData", dt);
    }

    // DragEvent — dataTransfer with files and text
    auto* dragEvt = dynamic_cast<bro::dom::DragEvent*>(&event);
    if (dragEvt && dragEvt->isSessionDrag()) {
        // An in-page drag: one DataTransfer for the whole gesture, created at
        // dragstart and stashed on the global. Reusing it is the point — what
        // the source writes in dragstart is what the drop handler reads back.
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue dt = JS_GetPropertyStr(ctx, global, "__bro_dragDataTransfer");
        JS_FreeValue(ctx, global);
        if (JS_IsObject(dt)) JS_SetPropertyStr(ctx, jsEvent, "dataTransfer", dt);
        else JS_FreeValue(ctx, dt);
        return;
    }
    if (dragEvt) {
        JSValue dt = JS_NewObject(ctx);
        // dataTransfer.getData("text/plain")
        JS_SetPropertyStr(ctx, dt, "_text",
                          JS_NewString(ctx, dragEvt->dataText().c_str()));
        JS_SetPropertyStr(ctx, dt, "getData", JS_NewCFunction2(ctx,
            [](JSContext* c, JSValueConst this_val, int, JSValueConst*) -> JSValue {
                return JS_GetPropertyStr(c, this_val, "_text");
            }, "getData", 1, JS_CFUNC_generic, 0));
        JS_SetPropertyStr(ctx, dt, "setData", JS_NewCFunction2(ctx,
            [](JSContext* c, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
                if (argc >= 2) {
                    JS_SetPropertyStr(c, this_val, "_text", JS_DupValue(c, argv[1]));
                }
                return JS_UNDEFINED;
            }, "setData", 2, JS_CFUNC_generic, 0));
        // dataTransfer.files — real File objects.
        //
        // The engine only knows the dropped paths; what the page expects is
        // Files it can hand to FileReader, URL.createObjectURL or fetch. The
        // File API polyfill's __bro_fileFromPath reads the bytes and builds
        // one. Anything unreadable (a directory, a file that vanished between
        // the drop and the dispatch, a permission error) falls back to the
        // name/path descriptor, so a drop never fails outright.
        auto& files = dragEvt->files();
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue fileFromPath = JS_GetPropertyStr(ctx, global, "__bro_fileFromPath");
        bool haveFileFromPath = JS_IsFunction(ctx, fileFromPath);
        JSValue filesArr = JS_NewArray(ctx);
        for (size_t i = 0; i < files.size(); i++) {
            JSValue fileVal = JS_UNDEFINED;
            if (haveFileFromPath) {
                JSValue arg = JS_NewString(ctx, files[i].c_str());
                fileVal = JS_Call(ctx, fileFromPath, JS_UNDEFINED, 1, &arg);
                JS_FreeValue(ctx, arg);
                if (JS_IsException(fileVal)) {
                    JS_FreeValue(ctx, JS_GetException(ctx));
                    fileVal = JS_UNDEFINED;
                } else if (JS_IsNull(fileVal)) {
                    JS_FreeValue(ctx, fileVal);
                    fileVal = JS_UNDEFINED;
                }
            }
            if (JS_IsUndefined(fileVal)) {
                // `name` is the basename and `path` the full location, as in the
                // real DataTransfer. Setting both to the path made every app that
                // displayed file.name — the obvious thing to display — print an
                // absolute path instead of a filename.
                fileVal = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, fileVal, "name",
                    JS_NewString(ctx, std::filesystem::path(files[i]).filename().string().c_str()));
                JS_SetPropertyStr(ctx, fileVal, "path",
                    JS_NewString(ctx, files[i].c_str()));
            }
            JS_SetPropertyInt64(ctx, filesArr, static_cast<int64_t>(i), fileVal);
        }
        JS_FreeValue(ctx, fileFromPath);
        JS_FreeValue(ctx, global);
        JS_SetPropertyStr(ctx, dt, "files", filesArr);

        // dataTransfer.types. Drop handlers branch on it before they touch
        // anything else — "Files" for a file drop, "text/plain" for dragged
        // text — and reading [0] off an undefined types threw before this.
        JSValue typesArr = JS_NewArray(ctx);
        uint32_t typeIdx = 0;
        if (!files.empty())
            JS_SetPropertyUint32(ctx, typesArr, typeIdx++, JS_NewString(ctx, "Files"));
        if (!dragEvt->dataText().empty())
            JS_SetPropertyUint32(ctx, typesArr, typeIdx++, JS_NewString(ctx, "text/plain"));
        JS_SetPropertyStr(ctx, dt, "types", typesArr);

        JS_SetPropertyStr(ctx, jsEvent, "dataTransfer", dt);
    }
}

// Stash the composedPath as a JS array on the event object
void stashComposedPath(JSContext* ctx, JSValue jsEvent,
                       const std::vector<EventPathEntry>& path) {
    JSValue arr = JS_NewArray(ctx);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");

    if (!JS_IsUndefined(elemMap)) {
        for (size_t i = 0; i < path.size(); ++i) {
            std::string key = std::to_string(path[i].element->nodeId());
            JSValue elem = JS_GetPropertyStr(ctx, elemMap, key.c_str());
            if (!JS_IsUndefined(elem) && !JS_IsNull(elem)) {
                JS_SetPropertyInt64(ctx, arr, static_cast<int64_t>(i), elem);
            } else {
                JS_FreeValue(ctx, elem);
            }
        }
    }

    JS_FreeValue(ctx, elemMap);
    JS_FreeValue(ctx, global);
    JS_SetPropertyStr(ctx, jsEvent, "_composedPath", arr);
}

// The four methods every JS event object carries. Shared by the element and
// window paths so a window event is not a poorer object than a DOM one.
void installJsEventMethods(JSContext* ctx, JSValue jsEvent) {
    JS_SetPropertyStr(ctx, jsEvent, "stopPropagation",
        JS_NewCFunction(ctx, js_ev_stopPropagation, "stopPropagation", 0));
    JS_SetPropertyStr(ctx, jsEvent, "preventDefault",
        JS_NewCFunction(ctx, js_ev_preventDefault, "preventDefault", 0));
    JS_SetPropertyStr(ctx, jsEvent, "stopImmediatePropagation",
        JS_NewCFunction(ctx, js_ev_stopImmediatePropagation,
                        "stopImmediatePropagation", 0));
    JS_SetPropertyStr(ctx, jsEvent, "composedPath",
        JS_NewCFunction(ctx, js_ev_composedPath, "composedPath", 0));
}

// Read the flags JS listeners set back onto the dom::Event.
void readJsFlagsBack(JSContext* ctx, JSValue jsEvent,
                     bro::dom::Event& event) {
    JSValue stoppedVal = JS_GetPropertyStr(ctx, jsEvent, "_stopped");
    if (JS_ToBool(ctx, stoppedVal)) event.stopPropagation();
    JS_FreeValue(ctx, stoppedVal);

    JSValue preventedVal = JS_GetPropertyStr(ctx, jsEvent, "_prevented");
    if (JS_ToBool(ctx, preventedVal)) event.preventDefault();
    JS_FreeValue(ctx, preventedVal);

    JSValue immVal = JS_GetPropertyStr(ctx, jsEvent, "_immediateStopped");
    if (JS_ToBool(ctx, immVal)) event.stopImmediatePropagation();
    JS_FreeValue(ctx, immVal);
}

bool jsEventStringDetail(JSContext* ctx, JSValue jsEvent, std::string& out) {
    if (!ctx || !JS_IsObject(jsEvent)) return false;
    JSValue detailVal = JS_GetPropertyStr(ctx, jsEvent, "detail");
    bool isString = JS_IsString(detailVal);
    if (isString) {
        const char* s = JS_ToCString(ctx, detailVal);
        out = s ? s : "";
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, detailVal);
    return isString;
}

} // namespace bro::js
