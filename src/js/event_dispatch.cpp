#include "js/event_dispatch.h"
#include "js/dom_bindings.h"
#include "js/dom_bindings_internal.h"
#include "js/runtime.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event_target.h"
#include "dom/shadow_root.h"
#include "dom/event.h"
#include "util/log.h"

#include <algorithm>
#include <string>
#include <cstring>
#include <filesystem>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

// Build a short "click on #my-id" / "click on div.foo" / "click on div"
// description used in JS error log lines for listener invocations.
static std::string describeListener(const std::string& evtType,
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

// C-function methods for plain JS event objects.  They set flag properties
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

// Event phases per DOM spec
static constexpr int NONE = 0;
static constexpr int CAPTURING_PHASE = 1;
static constexpr int AT_TARGET = 2;
static constexpr int BUBBLING_PHASE = 3;

// Build the event path from target up to the document root.
// Handles shadow DOM: when an element is inside a shadow root, the path
// crosses from shadow tree → host element → host's parent, etc.
// At each shadow boundary, the effective target is retargeted to the host.
struct EventPathEntry {
    bro::dom::Element* element;
    bro::dom::Element* retargetedTarget; // target visible at this scope
};

static std::vector<EventPathEntry> buildEventPath(bro::dom::Element* target) {
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

static void populateJsEvent(JSContext* ctx, JSValue jsEvent, bro::dom::Event& event) {
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
        // Legacy properties (deprecated but widely used)
        JS_SetPropertyStr(ctx, jsEvent, "keyCode",
                          JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, jsEvent, "charCode",
                          JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, jsEvent, "which",
                          JS_NewInt32(ctx, 0));
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
        // dataTransfer.files array
        auto& files = dragEvt->files();
        JSValue filesArr = JS_NewArray(ctx);
        for (size_t i = 0; i < files.size(); i++) {
            JSValue fileObj = JS_NewObject(ctx);
            // `name` is the basename and `path` the full location, as in the
            // real DataTransfer. Setting both to the path made every app that
            // displayed file.name — the obvious thing to display — print an
            // absolute path instead of a filename.
            JS_SetPropertyStr(ctx, fileObj, "name",
                JS_NewString(ctx, std::filesystem::path(files[i]).filename().string().c_str()));
            JS_SetPropertyStr(ctx, fileObj, "path",
                JS_NewString(ctx, files[i].c_str()));
            JS_SetPropertyInt64(ctx, filesArr, static_cast<int64_t>(i), fileObj);
        }
        JS_SetPropertyStr(ctx, dt, "files", filesArr);
        JS_SetPropertyStr(ctx, jsEvent, "dataTransfer", dt);
    }
}

// Stash the composedPath as a JS array on the event object
static void stashComposedPath(JSContext* ctx, JSValue jsEvent,
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

// ---------------------------------------------------------------------------
// C++ listeners
//
// C++ and JS listeners on one target fire in a single registration-ordered
// sequence: every registration on either side takes a number from
// dom::nextListenerSeq(), the JS bindings stash it on the listener record as
// `seq`, and dispatch merges the two lists on it. A C++ listener therefore
// sees exactly the interleaving the same code written in JS would have seen.
//
// preventDefault() / stopPropagation() / stopImmediatePropagation() called on
// the dom::Event by a C++ listener are mirrored onto the JS event object the
// remaining JS listeners receive, so cancellation crosses the boundary in
// both directions (the JS→C++ direction was already there: every JS listener
// invocation reads the flags back onto the dom::Event).
// ---------------------------------------------------------------------------

using NativeEntryPtr = bro::dom::NativeListenerList::EntryPtr;

static bool listenerRunsInPhase(int phase, bool capture) {
    return (phase == AT_TARGET) ||
           (phase == CAPTURING_PHASE && capture) ||
           (phase == BUBBLING_PHASE && !capture);
}

// Push flags a C++ listener set on the dom::Event onto the JS event object, so
// JS listeners later in the same dispatch observe them. Only ever sets: a JS
// listener cannot un-prevent an event either.
static void mirrorNativeFlagsToJs(JSContext* ctx, JSValue jsEvent,
                                  const bro::dom::Event& event) {
    if (!ctx || JS_IsUndefined(jsEvent) || !JS_IsObject(jsEvent)) return;
    if (event.defaultPrevented()) {
        JS_SetPropertyStr(ctx, jsEvent, "_prevented", JS_TRUE);
        JS_SetPropertyStr(ctx, jsEvent, "defaultPrevented", JS_TRUE);
    }
    if (event.propagationStopped())
        JS_SetPropertyStr(ctx, jsEvent, "_stopped", JS_TRUE);
    if (event.immediatePropagationStopped())
        JS_SetPropertyStr(ctx, jsEvent, "_immediateStopped", JS_TRUE);
}

// Invoke one C++ listener. Returns false when dispatch at this target must
// stop immediately (stopImmediatePropagation).
// `list` may be null; it is only needed to honour ListenerOptions::once.
static bool invokeNativeEntry(const NativeEntryPtr& entry,
                              bro::dom::NativeListenerList* list,
                              bro::dom::Event& event,
                              JSContext* ctx, JSValue jsEvent) {
    // Snapshots outlive removal: a listener removed by an earlier listener in
    // this same dispatch must not run.
    if (!entry || entry->removed || !entry->cb) return true;
    if (entry->opts.once && list) list->remove(bro::dom::ListenerHandle{entry->id});
    entry->cb(event);
    mirrorNativeFlagsToJs(ctx, jsEvent, event);
    return !event.immediatePropagationStopped();
}

// The four methods every JS event object carries. Shared by the element and
// window paths so a window event is not a poorer object than a DOM one.
static void installJsEventMethods(JSContext* ctx, JSValue jsEvent) {
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
static void readJsFlagsBack(JSContext* ctx, JSValue jsEvent,
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

// The registration sequence stamped on a JS listener record. Records written
// before this existed (or by code that did not stamp one) sort first, keeping
// the pre-existing "JS array order" behaviour for them.
static uint64_t jsListenerSeq(JSContext* ctx, JSValueConst entry) {
    JSValue seqVal = JS_GetPropertyStr(ctx, entry, "seq");
    int64_t seq = 0;
    if (JS_IsNumber(seqVal)) JS_ToInt64(ctx, &seq, seqVal);
    JS_FreeValue(ctx, seqVal);
    return seq < 0 ? 0 : static_cast<uint64_t>(seq);
}

// phase: CAPTURING_PHASE, AT_TARGET, or BUBBLING_PHASE
static void invokeListeners(JSContext* ctx, bro::dom::Element* current,
                            bro::dom::Element* retargetedTarget,
                            bro::dom::Event& event,
                            int phase,
                            JSValue originalJsEvent = JS_UNDEFINED) {
    // --- C++ listeners on this element, for this type and phase -------------
    // Gathered first: they are the only listeners that can run when the realm
    // has no JSContext, and their presence decides whether the JS loop below
    // has to merge or can stay on its existing fast path.
    auto* nativeList = current->nativeListeners();
    std::vector<NativeEntryPtr> nativeEntries;
    if (nativeList) {
        for (auto& e : nativeList->snapshot(event.type()))
            if (listenerRunsInPhase(phase, e->opts.capture))
                nativeEntries.push_back(std::move(e));
    }

    // A C++ listener sees the target the way this scope sees it — retargeted
    // to the shadow host outside the shadow tree, exactly like the JS side's
    // event.target. Restored afterwards so the next scope retargets from the
    // real target.
    bro::dom::Element* savedTarget = event.target();
    struct TargetRestore {
        bro::dom::Event& ev; bro::dom::Element* saved;
        ~TargetRestore() { ev.setTarget(saved); }
    } targetRestore{event, savedTarget};
    if (retargetedTarget) event.setTarget(retargetedTarget);

    if (!ctx) {
        // Realm with no JS: C++ listeners are the whole of dispatch. Inline
        // on* attributes and el.onclick handlers are JS by definition and
        // cannot run here.
        for (auto& e : nativeEntries)
            if (!invokeNativeEntry(e, nativeList, event, nullptr, JS_UNDEFINED)) break;
        return;
    }

    // Check if this element has registered listeners OR an inline handler
    bool hasListeners = current->hasJsListener(event.type());
    bool hasInlineHandler = false;
    std::string attrName;
    if (phase == AT_TARGET || phase == BUBBLING_PHASE) {
        attrName = "on" + event.type();
        hasInlineHandler = !current->getAttribute(attrName).empty();
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (JS_IsUndefined(elemMap)) {
        // Map doesn't exist yet (no JS has accessed any DOM element).
        // Create it so inline handlers and wrapElement can use it.
        elemMap = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__bro_elem_map", JS_DupValue(ctx, elemMap));
    }

    std::string elemKey = std::to_string(current->nodeId());
    JSValue jsElem = JS_GetPropertyStr(ctx, elemMap, elemKey.c_str());
    JS_FreeValue(ctx, elemMap);

    // Check for IDL event-handler property (e.g. el.onclick = fn). Only possible
    // if a JS wrapper already exists; a fresh wrapper would have no such prop.
    bool hasPropertyHandler = false;
    if (!attrName.empty() && !JS_IsUndefined(jsElem) && !JS_IsNull(jsElem)) {
        JSValue propHandler = JS_GetPropertyStr(ctx, jsElem, attrName.c_str());
        hasPropertyHandler = JS_IsFunction(ctx, propHandler);
        JS_FreeValue(ctx, propHandler);
    }

    if (!hasListeners && !hasInlineHandler && !hasPropertyHandler &&
        nativeEntries.empty()) {
        JS_FreeValue(ctx, jsElem);
        JS_FreeValue(ctx, global);
        return;
    }

    if (JS_IsUndefined(jsElem) || JS_IsNull(jsElem)) {
        JS_FreeValue(ctx, jsElem);
        // Element has no JS wrapper yet. Create one on demand so that
        // inline event handler attributes (onclick, etc.) can fire.
        jsElem = DomBindings::wrapElement(ctx, current);
        if (JS_IsUndefined(jsElem) || JS_IsException(jsElem)) {
            JS_FreeValue(ctx, jsElem);
            JS_FreeValue(ctx, global);
            return;
        }
    }

    JSValue listenersArr = JS_GetPropertyStr(ctx, jsElem, "__bro_listeners");
    bool hasListenersArr = !JS_IsUndefined(listenersArr) && JS_IsArray(listenersArr);

    int64_t len = 0;
    if (hasListenersArr) {
        JSValue lenVal = JS_GetPropertyStr(ctx, listenersArr, "length");
        JS_ToInt64(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
    }


    // Build the JS event object — reuse original if provided (preserves detail, etc.)
    bool ownsEvent = JS_IsUndefined(originalJsEvent);
    JSValue jsEvent;
    if (ownsEvent) {
        jsEvent = JS_NewObject(ctx);
        populateJsEvent(ctx, jsEvent, event);
    } else {
        jsEvent = JS_DupValue(ctx, originalJsEvent);
    }

    // Set eventPhase
    JS_SetPropertyStr(ctx, jsEvent, "eventPhase", JS_NewInt32(ctx, phase));

    // Set currentTarget to the current element
    JS_SetPropertyStr(ctx, jsEvent, "currentTarget", JS_DupValue(ctx, jsElem));

    // Set target to the retargeted target (may be the shadow host from outside)
    {
        JSValue tgtGlobal = JS_GetGlobalObject(ctx);
        JSValue tgtMap = JS_GetPropertyStr(ctx, tgtGlobal, "__bro_elem_map");
        if (!JS_IsUndefined(tgtMap) && retargetedTarget) {
            std::string tgtKey = std::to_string(retargetedTarget->nodeId());
            JSValue tgtElem = JS_GetPropertyStr(ctx, tgtMap, tgtKey.c_str());
            if (JS_IsUndefined(tgtElem) || JS_IsNull(tgtElem)) {
                JS_FreeValue(ctx, tgtElem);
                tgtElem = DomBindings::wrapElement(ctx, retargetedTarget);
            }
            JS_SetPropertyStr(ctx, jsEvent, "target", tgtElem);
        } else {
            JS_SetPropertyStr(ctx, jsEvent, "target", JS_DupValue(ctx, jsElem));
        }
        JS_FreeValue(ctx, tgtMap);
        JS_FreeValue(ctx, tgtGlobal);
    }

    installJsEventMethods(ctx, jsEvent);

    // Collect indices of "once" listeners to remove after dispatch
    std::vector<int64_t> onceIndices;

    // Invoke the JS listener record at array index `i`. Returns false when
    // dispatch at this element must stop (stopImmediatePropagation).
    auto invokeJsEntryAt = [&](int64_t i) -> bool {
        JSValue entry = JS_GetPropertyInt64(ctx, listenersArr, i);
        if (JS_IsObject(entry)) {
            JSValue typeVal = JS_GetPropertyStr(ctx, entry, "type");
            const char* entryType = JS_ToCString(ctx, typeVal);
            bool match = entryType && event.type() == entryType;
            JS_FreeCString(ctx, entryType);
            JS_FreeValue(ctx, typeVal);

            if (match) {
                // Check capture flag on listener
                JSValue captureVal = JS_GetPropertyStr(ctx, entry, "capture");
                bool isCapture = JS_ToBool(ctx, captureVal);
                JS_FreeValue(ctx, captureVal);

                // During capture phase, only invoke capture listeners.
                // During bubble phase, only invoke non-capture listeners.
                // At target, invoke all listeners regardless of capture flag.
                bool shouldInvoke = (phase == AT_TARGET) ||
                                    (phase == CAPTURING_PHASE && isCapture) ||
                                    (phase == BUBBLING_PHASE && !isCapture);

                if (shouldInvoke) {
                    JSValue cb = JS_GetPropertyStr(ctx, entry, "cb");
                    if (JS_IsFunction(ctx, cb)) {
                        JSValue result = Runtime::callJs(ctx, cb, jsElem, 1, &jsEvent,
                            ErrorOrigin::listener(describeListener(event.type(), current)));
                        JS_FreeValue(ctx, result);
                    }
                    JS_FreeValue(ctx, cb);

                    // Check if this is a "once" listener
                    JSValue onceVal = JS_GetPropertyStr(ctx, entry, "once");
                    if (JS_ToBool(ctx, onceVal)) {
                        onceIndices.push_back(i);
                    }
                    JS_FreeValue(ctx, onceVal);

                    // Check propagation flags
                    JSValue stoppedVal = JS_GetPropertyStr(ctx, jsEvent, "_stopped");
                    if (JS_ToBool(ctx, stoppedVal))
                        event.stopPropagation();
                    JS_FreeValue(ctx, stoppedVal);

                    // Check preventDefault from JS side
                    JSValue preventedVal = JS_GetPropertyStr(ctx, jsEvent, "_prevented");
                    if (JS_ToBool(ctx, preventedVal))
                        event.preventDefault();
                    JS_FreeValue(ctx, preventedVal);

                    JSValue immVal = JS_GetPropertyStr(ctx, jsEvent, "_immediateStopped");
                    bool immStopped = JS_ToBool(ctx, immVal);
                    JS_FreeValue(ctx, immVal);
                    if (immStopped) {
                        event.stopImmediatePropagation();
                        JS_FreeValue(ctx, entry);
                        return false;
                    }
                }
            }
        }
        JS_FreeValue(ctx, entry);
        return true;
    };

    if (nativeEntries.empty()) {
        // No C++ listeners here — the ordinary path, unchanged.
        for (int64_t i = 0; i < len; i++) {
            if (!invokeJsEntryAt(i)) break;
            if (event.propagationStopped()) break;
        }
    } else {
        // Merge the two lists on the shared registration sequence so C++ and
        // JS listeners on this element fire in the order they were added.
        struct Slot { uint64_t seq; int64_t jsIndex; const NativeEntryPtr* native; };
        std::vector<Slot> slots;
        slots.reserve(static_cast<size_t>(len) + nativeEntries.size());
        for (int64_t i = 0; i < len; i++) {
            JSValue entry = JS_GetPropertyInt64(ctx, listenersArr, i);
            if (JS_IsObject(entry)) {
                JSValue typeVal = JS_GetPropertyStr(ctx, entry, "type");
                const char* entryType = JS_ToCString(ctx, typeVal);
                bool match = entryType && event.type() == entryType;
                JS_FreeCString(ctx, entryType);
                JS_FreeValue(ctx, typeVal);
                if (match) slots.push_back({jsListenerSeq(ctx, entry), i, nullptr});
            }
            JS_FreeValue(ctx, entry);
        }
        for (const auto& e : nativeEntries) slots.push_back({e->seq, -1, &e});
        std::stable_sort(slots.begin(), slots.end(),
                         [](const Slot& a, const Slot& b) { return a.seq < b.seq; });

        for (const auto& slot : slots) {
            if (slot.native) {
                if (!invokeNativeEntry(*slot.native, nativeList, event, ctx, jsEvent))
                    break;
            } else {
                if (!invokeJsEntryAt(slot.jsIndex)) break;
            }
            if (event.propagationStopped()) break;
        }
    }

    // Remove "once" listeners by compacting the array (splice out holes)
    if (!onceIndices.empty()) {
        // Mark slots as undefined
        for (auto it2 = onceIndices.rbegin(); it2 != onceIndices.rend(); ++it2) {
            JS_SetPropertyInt64(ctx, listenersArr, *it2, JS_UNDEFINED);
        }
        // Compact: shift valid entries down, then truncate
        int64_t dst = 0;
        for (int64_t src = 0; src < len; ++src) {
            JSValue v = JS_GetPropertyInt64(ctx, listenersArr, src);
            if (!JS_IsUndefined(v)) {
                if (dst != src)
                    JS_SetPropertyInt64(ctx, listenersArr, dst, v);
                else
                    JS_FreeValue(ctx, v);
                ++dst;
            } else {
                JS_FreeValue(ctx, v);
            }
        }
        // Truncate by setting length
        JS_SetPropertyStr(ctx, listenersArr, "length", JS_NewInt64(ctx, dst));
        // A reaped `once` listener is gone for the same reasons an explicitly
        // removed one is, so the element's per-type count has to come down with
        // it — otherwise firing a one-shot listener would leave its type looking
        // permanently subscribed. Every index in onceIndices matched
        // event.type() to be invoked at all.
        for (size_t k = 0; k < onceIndices.size(); ++k)
            current->removeJsListener(event.type());
    }

    // --- Inline event handler: IDL property (el.onclick = fn) ---
    // Per DOM spec, fires after registered listeners during AT_TARGET/BUBBLING.
    // Prefer the JS property over the HTML attribute when both are set (matches
    // browser behavior: assigning el.onclick overrides the attribute).
    bool propertyHandlerFired = false;
    if ((phase == AT_TARGET || phase == BUBBLING_PHASE) && !event.propagationStopped()) {
        JSValue propHandler = JS_GetPropertyStr(ctx, jsElem, attrName.c_str());
        if (JS_IsFunction(ctx, propHandler)) {
            JSValue result = Runtime::callJs(ctx, propHandler, jsElem, 1, &jsEvent,
                ErrorOrigin::listener(describeListener(event.type(), current) + " (." + attrName + ")"));
            if (JS_IsBool(result) && !JS_ToBool(ctx, result)) {
                event.preventDefault();
            }
            JS_FreeValue(ctx, result);
            propertyHandlerFired = true;

            JSValue stoppedVal = JS_GetPropertyStr(ctx, jsEvent, "_stopped");
            if (JS_ToBool(ctx, stoppedVal))
                event.stopPropagation();
            JS_FreeValue(ctx, stoppedVal);

            JSValue preventedVal = JS_GetPropertyStr(ctx, jsEvent, "_prevented");
            if (JS_ToBool(ctx, preventedVal))
                event.preventDefault();
            JS_FreeValue(ctx, preventedVal);
        }
        JS_FreeValue(ctx, propHandler);
    }

    // --- Inline event handler attributes (onclick, onmouseover, etc.) ---
    // Per DOM spec, inline handlers fire during AT_TARGET or BUBBLING phase,
    // after any registered listeners on the same element. Skipped when an IDL
    // property handler already fired (the property overrides the attribute).
    if ((phase == AT_TARGET || phase == BUBBLING_PHASE) && !event.propagationStopped() && !propertyHandlerFired) {
        std::string handlerCode = current->getAttribute(attrName);
        if (!handlerCode.empty()) {
            // Compile the handler as a function body with 'event' parameter.
            // 'this' is bound to the element.
            std::string funcSource = "(function(event){" + handlerCode + "\n})";
            JSValue func = JS_Eval(ctx, funcSource.c_str(), funcSource.size(),
                                   attrName.c_str(), JS_EVAL_TYPE_GLOBAL);
            if (JS_IsFunction(ctx, func)) {
                JSValue result = Runtime::callJs(ctx, func, jsElem, 1, &jsEvent,
                    ErrorOrigin::listener(describeListener(event.type(), current) + " (" + attrName + " attr)"));
                // onclick returning false means preventDefault
                if (JS_IsBool(result) && !JS_ToBool(ctx, result)) {
                    event.preventDefault();
                }
                JS_FreeValue(ctx, result);
            } else if (JS_IsException(func)) {
                Runtime::checkException(ctx, func, ErrorOrigin::eval(attrName));
            }
            JS_FreeValue(ctx, func);

            // Check propagation flags set by handler
            JSValue stoppedVal2 = JS_GetPropertyStr(ctx, jsEvent, "_stopped");
            if (JS_ToBool(ctx, stoppedVal2))
                event.stopPropagation();
            JS_FreeValue(ctx, stoppedVal2);

            JSValue preventedVal2 = JS_GetPropertyStr(ctx, jsEvent, "_prevented");
            if (JS_ToBool(ctx, preventedVal2))
                event.preventDefault();
            JS_FreeValue(ctx, preventedVal2);
        }
    }

    JS_FreeValue(ctx, jsEvent);
    JS_FreeValue(ctx, listenersArr);
    JS_FreeValue(ctx, jsElem);
    JS_FreeValue(ctx, global);
}

// ---------------------------------------------------------------------------
// Window dispatch
//
// A realm's window listeners live in two places: the JS ones in the window
// polyfill's __bro_win_listeners side map, the C++ ones in the realm
// Document's windowListeners(). This is the single loop that runs both, in
// registration order (see the block above invokeListeners).
//
// It is also what globalThis.__bro_dispatch_window_event is bound to — the
// polyfill no longer implements that function — so every existing dispatch
// site, C++ (resize, gamepad, message, DOMContentLoaded) and JS (popstate,
// hashchange, visibilitychange, window.dispatchEvent) alike, reaches C++
// listeners without knowing about them.
// ---------------------------------------------------------------------------

// captureFilter: 1 = capture listeners only, 0 = bubble listeners only,
// -1 = both (the legacy one-shot dispatch sites, and window.dispatchEvent).
static void dispatchWindowEventCore(JSContext* ctx, bro::dom::Document* doc,
                                    bro::dom::Event& event, JSValue originalJsEvent,
                                    int captureFilter, bro::dom::Element* target) {
    if (!doc && ctx) doc = getDocumentForCtx(ctx);
    // No JS realm to ask: the target element knows its document, and that
    // document owns the realm's C++ window listeners.
    if (!doc && target) doc = target->document();

    std::vector<NativeEntryPtr> nativeEntries;
    if (doc) {
        for (auto& e : doc->windowListeners().snapshot(event.type())) {
            if (captureFilter >= 0 && e->opts.capture != (captureFilter == 1)) continue;
            nativeEntries.push_back(std::move(e));
        }
    }
    auto* nativeList = doc ? &doc->windowListeners() : nullptr;

    if (!ctx) {
        // Realm with no JS: only the C++ listeners exist.
        for (auto& e : nativeEntries)
            if (!invokeNativeEntry(e, nativeList, event, nullptr, JS_UNDEFINED)) break;
        return;
    }

    JSValue global = JS_GetGlobalObject(ctx);

    // The realm's JS window listener array for this type.
    JSValue winMap = JS_GetPropertyStr(ctx, global, "__bro_win_listeners");
    JSValue liveArr = JS_UNDEFINED;
    if (JS_IsObject(winMap))
        liveArr = JS_GetPropertyStr(ctx, winMap, event.type().c_str());
    JS_FreeValue(ctx, winMap);
    if (!JS_IsArray(liveArr)) {
        JS_FreeValue(ctx, liveArr);
        liveArr = JS_UNDEFINED;
    }

    if (nativeEntries.empty() && JS_IsUndefined(liveArr)) {
        JS_FreeValue(ctx, liveArr);
        JS_FreeValue(ctx, global);
        return;
    }

    bool ownsEvent = JS_IsUndefined(originalJsEvent);
    JSValue jsEvent;
    if (ownsEvent) {
        jsEvent = JS_NewObject(ctx);
        populateJsEvent(ctx, jsEvent, event);
        installJsEventMethods(ctx, jsEvent);

        // Resolve target to its JS wrapper. Window events fired at the window
        // itself have no element target; the window is the target then.
        JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
        if (!JS_IsUndefined(elemMap) && target) {
            std::string tgtKey = std::to_string(target->nodeId());
            JSValue tgtElem = JS_GetPropertyStr(ctx, elemMap, tgtKey.c_str());
            if (JS_IsUndefined(tgtElem) || JS_IsNull(tgtElem)) {
                JS_FreeValue(ctx, tgtElem);
                tgtElem = DomBindings::wrapElement(ctx, target);
            }
            JS_SetPropertyStr(ctx, jsEvent, "target", tgtElem);
        } else if (target) {
            JS_SetPropertyStr(ctx, jsEvent, "target", JS_NULL);
        } else {
            JS_SetPropertyStr(ctx, jsEvent, "target", JS_DupValue(ctx, global));
        }
        JS_FreeValue(ctx, elemMap);
    } else {
        jsEvent = JS_DupValue(ctx, originalJsEvent);
    }

    JS_SetPropertyStr(ctx, jsEvent, "currentTarget", JS_DupValue(ctx, global));
    if (captureFilter >= 0) {
        JS_SetPropertyStr(ctx, jsEvent, "eventPhase",
            JS_NewInt32(ctx, captureFilter == 1 ? CAPTURING_PHASE : BUBBLING_PHASE));
    }

    // Snapshot the JS listener records the way the polyfill used to: a handler
    // may add or remove listeners (commonly its own) mid-dispatch, and neither
    // may corrupt this iteration.
    struct JsSlot { JSValue entry; uint64_t seq; };
    std::vector<JsSlot> jsSlots;
    int64_t liveLen = 0;
    if (!JS_IsUndefined(liveArr)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, liveArr, "length");
        JS_ToInt64(ctx, &liveLen, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int64_t i = 0; i < liveLen; ++i) {
            JSValue e = JS_GetPropertyInt64(ctx, liveArr, i);
            if (JS_IsObject(e)) jsSlots.push_back({e, jsListenerSeq(ctx, e)});
            else JS_FreeValue(ctx, e);
        }
    }

    // Index of `entry` in the live array, or -1 if it has been removed.
    auto liveIndexOf = [&](JSValueConst entry) -> int64_t {
        if (JS_IsUndefined(liveArr)) return -1;
        int64_t len = 0;
        JSValue lenVal = JS_GetPropertyStr(ctx, liveArr, "length");
        JS_ToInt64(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        for (int64_t i = 0; i < len; ++i) {
            JSValue e = JS_GetPropertyInt64(ctx, liveArr, i);
            bool same = JS_VALUE_GET_PTR(e) == JS_VALUE_GET_PTR(entry);
            JS_FreeValue(ctx, e);
            if (same) return i;
        }
        return -1;
    };

    struct Slot { uint64_t seq; int jsSlot; const NativeEntryPtr* native; };
    std::vector<Slot> slots;
    slots.reserve(jsSlots.size() + nativeEntries.size());
    for (size_t i = 0; i < jsSlots.size(); ++i)
        slots.push_back({jsSlots[i].seq, static_cast<int>(i), nullptr});
    for (const auto& e : nativeEntries) slots.push_back({e->seq, -1, &e});
    std::stable_sort(slots.begin(), slots.end(),
                     [](const Slot& a, const Slot& b) { return a.seq < b.seq; });

    for (const auto& slot : slots) {
        if (slot.native) {
            if (!invokeNativeEntry(*slot.native, nativeList, event, ctx, jsEvent)) break;
            continue;
        }
        JSValue entry = jsSlots[static_cast<size_t>(slot.jsSlot)].entry;

        if (captureFilter >= 0) {
            JSValue capVal = JS_GetPropertyStr(ctx, entry, "capture");
            bool isCapture = JS_ToBool(ctx, capVal);
            JS_FreeValue(ctx, capVal);
            if (isCapture != (captureFilter == 1)) continue;
        }
        if (liveIndexOf(entry) < 0) continue;   // removed since the snapshot

        JSValue fn = JS_GetPropertyStr(ctx, entry, "fn");
        if (JS_IsFunction(ctx, fn)) {
            JSValue ret = Runtime::callJs(ctx, fn, global, 1, &jsEvent,
                ErrorOrigin::listener(event.type() + " on window"));
            JS_FreeValue(ctx, ret);
        }
        JS_FreeValue(ctx, fn);

        JSValue onceVal = JS_GetPropertyStr(ctx, entry, "once");
        bool once = JS_ToBool(ctx, onceVal);
        JS_FreeValue(ctx, onceVal);
        if (once) {
            int64_t idx = liveIndexOf(entry);
            if (idx >= 0) {
                int64_t len = 0;
                JSValue lenVal = JS_GetPropertyStr(ctx, liveArr, "length");
                JS_ToInt64(ctx, &len, lenVal);
                JS_FreeValue(ctx, lenVal);
                for (int64_t j = idx; j < len - 1; ++j) {
                    JSValue next = JS_GetPropertyInt64(ctx, liveArr, j + 1);
                    JS_SetPropertyInt64(ctx, liveArr, j, next);
                }
                JS_SetPropertyStr(ctx, liveArr, "length", JS_NewInt64(ctx, len - 1));
            }
        }

        readJsFlagsBack(ctx, jsEvent, event);
        if (event.immediatePropagationStopped()) break;
    }

    for (auto& s : jsSlots) JS_FreeValue(ctx, s.entry);
    JS_FreeValue(ctx, liveArr);
    JS_FreeValue(ctx, jsEvent);
    JS_FreeValue(ctx, global);
}

// Dispatch event to window-level listeners (set on globalThis via
// addEventListener). Per DOM spec, window is the outermost node in the
// propagation path: it receives capture first and bubble last.
static void dispatchToWindow(JSContext* ctx, bro::dom::Element* target,
                             bro::dom::Event& event,
                             JSValue originalJsEvent, bool isCapture) {
    dispatchWindowEventCore(ctx, nullptr, event, originalJsEvent,
                            isCapture ? 1 : 0, target);
}

void dispatchWindowEvent(JSContext* ctx, bro::dom::Document* doc,
                         bro::dom::Event& event, JSValue originalJsEvent) {
    dispatchWindowEventCore(ctx, doc, event, originalJsEvent,
                            /*captureFilter=*/-1, /*target=*/nullptr);
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

// globalThis.__bro_listener_seq() — the shared registration counter, so the
// window polyfill can stamp its listener records with the same sequence C++
// registrations take. Without it, C++ and JS window listeners could not be
// ordered against each other.
static JSValue js_listener_seq(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewInt64(ctx, static_cast<int64_t>(bro::dom::nextListenerSeq()));
}

// globalThis.__bro_dispatch_window_event(type, event, capture)
//
// Same signature the polyfill used to define, so every existing caller is
// unchanged. C++ listeners get a real dom::Event synthesized from the JS one:
// its type, bubbles/cancelable and defaultPrevented cross over, and anything
// the C++ listener does to it (preventDefault, stopImmediatePropagation)
// crosses back onto the JS object. Payload a JS caller put on the event —
// CustomEvent.detail, gamepad, PopStateEvent.state — is NOT visible on the
// C++ side; a C++ listener that needs it must be registered for an event the
// host itself dispatches (js::dispatchWindowEvent), which carries the real
// dom::Event through.
static JSValue js_dispatch_window_event(JSContext* ctx, JSValueConst,
                                        int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char* typeC = JS_ToCString(ctx, argv[0]);
    if (!typeC) return JS_UNDEFINED;
    std::string type(typeC);
    JS_FreeCString(ctx, typeC);

    JSValue jsEvent = (argc >= 2) ? argv[1] : JS_UNDEFINED;

    int captureFilter = -1;
    if (argc >= 3 && !JS_IsUndefined(argv[2]))
        captureFilter = JS_ToBool(ctx, argv[2]) ? 1 : 0;

    bool bubbles = false, cancelable = true, trusted = false, prevented = false;
    if (JS_IsObject(jsEvent)) {
        JSValue v = JS_GetPropertyStr(ctx, jsEvent, "bubbles");
        if (!JS_IsUndefined(v)) bubbles = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, jsEvent, "cancelable");
        if (!JS_IsUndefined(v)) cancelable = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, jsEvent, "isTrusted");
        trusted = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, jsEvent, "defaultPrevented");
        prevented = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
    }

    // A string `detail` is promoted to a real dom::CustomEvent so the realm's
    // C++ window listeners see the payload; anything else stays a plain event
    // for them, and the JS listeners get the caller's object regardless.
    std::string detail;
    const bool hasDetail = jsEventStringDetail(ctx, jsEvent, detail);
    bro::dom::CustomEvent customEvent(type, bubbles, cancelable);
    bro::dom::Event plainEvent(type, bubbles, cancelable);
    bro::dom::Event& event = hasDetail ? static_cast<bro::dom::Event&>(customEvent)
                                       : plainEvent;
    if (hasDetail) customEvent.setDetail(detail);
    event.setIsTrusted(trusted);
    if (prevented) event.preventDefault();

    dispatchWindowEventCore(ctx, nullptr, event, jsEvent, captureFilter, nullptr);
    return JS_UNDEFINED;
}

void installWindowEventDispatch(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__bro_listener_seq",
        JS_NewCFunction(ctx, js_listener_seq, "__bro_listener_seq", 0));
    JS_SetPropertyStr(ctx, global, "__bro_dispatch_window_event",
        JS_NewCFunction(ctx, js_dispatch_window_event,
                        "__bro_dispatch_window_event", 3));
    JS_FreeValue(ctx, global);
}

void dispatchDomEvent(JSContext* ctx, bro::dom::Element* target, bro::dom::Event& event,
                      JSValue originalJsEvent) {
    // ctx may be null: a realm with no JS still dispatches to the C++
    // listeners on the path, through this same algorithm.
    if (!target) return;

    event.setTarget(target);

    // Build the full event path including shadow DOM retargeting.
    // path[0] = target, path[N-1] = root ancestor
    auto path = buildEventPath(target);
    if (path.empty()) return;

    // Stash composed path on the original JS event if provided
    if (ctx && !JS_IsUndefined(originalJsEvent)) {
        stashComposedPath(ctx, originalJsEvent, path);
    }

    // --- Capture phase: window → root → target (exclusive) ---
    // Window sits outside the DOM tree but is the outermost EventTarget per
    // spec, so it captures first.
    if (!event.propagationStopped()) {
        dispatchToWindow(ctx, target, event, originalJsEvent, /*isCapture=*/true);
    }
    for (int i = static_cast<int>(path.size()) - 1; i > 0; --i) {
        if (event.propagationStopped()) break;
        event.setCurrentTarget(path[i].element);
        event.setEventPhase(CAPTURING_PHASE);
        invokeListeners(ctx, path[i].element, path[i].retargetedTarget,
                        event, CAPTURING_PHASE, originalJsEvent);
    }

    // --- At-target phase ---
    if (!event.propagationStopped()) {
        event.setCurrentTarget(path[0].element);
        event.setEventPhase(AT_TARGET);
        invokeListeners(ctx, path[0].element, path[0].retargetedTarget,
                        event, AT_TARGET, originalJsEvent);
    }

    // --- Bubble phase: target parent → root → window ---
    if (event.bubbles()) {
        for (size_t i = 1; i < path.size(); ++i) {
            if (event.propagationStopped()) break;
            event.setCurrentTarget(path[i].element);
            event.setEventPhase(BUBBLING_PHASE);
            invokeListeners(ctx, path[i].element, path[i].retargetedTarget,
                            event, BUBBLING_PHASE, originalJsEvent);
        }
        if (!event.propagationStopped()) {
            dispatchToWindow(ctx, target, event, originalJsEvent, /*isCapture=*/false);
        }
    }

    // Reset phase after dispatch
    event.setEventPhase(NONE);
    event.setCurrentTarget(nullptr);
}

} // namespace bro::js
