#include "js/dom_bindings.h"
#include "js/dom_bindings_internal.h"
#include "js/custom_elements.h"
#include "dom/document.h"

#include <cstring>

namespace bro::js {

// ===========================================================================
// Class IDs
// ===========================================================================

JSClassID js_document_class_id = 0;
JSClassID js_element_class_id  = 0;
JSClassID js_node_class_id    = 0;
JSClassID js_event_class_id    = 0;
JSClassID js_nodelist_class_id = 0;
JSClassID js_cssstyle_class_id = 0;
JSClassID js_computed_class_id = 0;
JSClassID js_tokenlist_class_id = 0;
JSClassID js_shadowroot_class_id = 0;
JSClassID js_htmlcollection_class_id = 0;

// ===========================================================================
// Per-context state
// ===========================================================================

std::unordered_map<JSRuntime*, bool> s_classes_registered;
std::unordered_map<JSContext*, bro::dom::Document*> s_ctx_documents;
std::unordered_map<JSContext*, DomBindings::GetContextFactory> s_ctx_factories;

bro::dom::Document* getDocumentForCtx(JSContext* ctx) {
    auto it = s_ctx_documents.find(ctx);
    return it != s_ctx_documents.end() ? it->second : nullptr;
}

void DomBindings::setGetContextFactory(JSContext* ctx, GetContextFactory factory) {
    s_ctx_factories[ctx] = std::move(factory);
}

// ===========================================================================
// Wrap / unwrap helpers (public API)
// ===========================================================================

JSValue DomBindings::wrapElement(JSContext* ctx, void* element_ptr)
{
    if (!element_ptr) return JS_NULL;

    auto* elem = static_cast<bro::dom::Element*>(element_ptr);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (JS_IsUndefined(elemMap)) {
        elemMap = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "__bro_elem_map", JS_DupValue(ctx, elemMap));
    }

    std::string key = std::to_string(elem->nodeId());
    JSValue existing = JS_GetPropertyStr(ctx, elemMap, key.c_str());
    if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) {
        JS_FreeValue(ctx, elemMap);
        JS_FreeValue(ctx, global);
        return existing;
    }
    JS_FreeValue(ctx, existing);

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_element_class_id));
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, elemMap);
        JS_FreeValue(ctx, global);
        return obj;
    }
    JS_SetOpaque(obj, element_ptr);

    upgradeCustomElementPrototype(ctx, obj, elem->tagName());

    JS_SetPropertyStr(ctx, elemMap, key.c_str(), JS_DupValue(ctx, obj));

    JS_FreeValue(ctx, elemMap);
    JS_FreeValue(ctx, global);
    return obj;
}

void* DomBindings::unwrapElement(JSContext* /*ctx*/, JSValueConst val)
{
    return JS_GetOpaque(val, js_element_class_id);
}

JSClassID DomBindings::elementClassId()
{
    return js_element_class_id;
}

JSValue DomBindings::wrapDocument(JSContext* ctx, void* document_ptr)
{
    if (!document_ptr) return JS_NULL;

    JSValue obj = JS_NewObjectClass(ctx, static_cast<int>(js_document_class_id));
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, document_ptr);
    return obj;
}

// ===========================================================================
// install() – register everything
// ===========================================================================

void DomBindings::install(JSContext* ctx, void* document_ptr)
{
    JSRuntime* rt = JS_GetRuntime(ctx);

    // ----- Allocate class IDs (idempotent — no-op if already non-zero) -----
    JS_NewClassID(rt, &js_document_class_id);
    JS_NewClassID(rt, &js_element_class_id);
    JS_NewClassID(rt, &js_node_class_id);
    JS_NewClassID(rt, &js_event_class_id);
    JS_NewClassID(rt, &js_nodelist_class_id);
    JS_NewClassID(rt, &js_cssstyle_class_id);
    JS_NewClassID(rt, &js_computed_class_id);
    JS_NewClassID(rt, &js_tokenlist_class_id);
    JS_NewClassID(rt, &js_shadowroot_class_id);
    JS_NewClassID(rt, &js_htmlcollection_class_id);

    // ----- Register classes on the runtime (once per runtime) -----
    if (!s_classes_registered[rt]) {
        registerDocumentClasses(rt);
        registerElementClasses(rt);
        registerNodeClasses(rt);
        registerEventClasses(rt);
        registerStyleClasses(rt);
        registerShadowRootClasses(rt);
        s_classes_registered[rt] = true;
    }

    // ----- Create prototypes (per-context via JS_SetClassProto) -----
    installDocumentPrototypes(ctx);
    installElementPrototypes(ctx);
    installNodePrototypes(ctx);
    installEventPrototypes(ctx);
    installStylePrototypes(ctx);
    installShadowRootPrototypes(ctx);

    // ----- Stash Document pointer for orphan management (per-context) -----
    s_ctx_documents[ctx] = static_cast<bro::dom::Document*>(document_ptr);

    // ----- Set global `document` -----
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue docObj = wrapDocument(ctx, document_ptr);
    JS_SetPropertyStr(ctx, global, "document", docObj);
    JS_FreeValue(ctx, global);

    // ----- Polyfills for jQuery/framework compatibility -----
    const char* polyfills = R"JS(
(function() {
    // document.implementation.createHTMLDocument
    document.implementation = {
        createHTMLDocument: function(title) {
            var body = document.createElement('body');
            var html = document.createElement('html');
            html.appendChild(body);
            return {
                nodeType: 9,
                documentElement: html,
                body: body,
                createElement: function(tag) { return document.createElement(tag); },
                createElementNS: function(ns, tag) { return document.createElementNS(ns, tag); },
                createTextNode: function(text) { return document.createTextNode(text); },
                createDocumentFragment: function() { return document.createDocumentFragment(); }
            };
        }
    };

    // Array.from polyfill (QuickJS may not have it)
    if (!Array.from) {
        Array.from = function(obj, mapFn) {
            var arr = [];
            for (var i = 0; i < obj.length; i++) arr.push(mapFn ? mapFn(obj[i], i) : obj[i]);
            return arr;
        };
    }

    // NodeList.prototype.forEach
    if (typeof NodeList !== 'undefined' && !NodeList.prototype.forEach) {
        NodeList.prototype.forEach = Array.prototype.forEach;
    }

    // Stub DOM type constructors needed by Vue and other frameworks
    if (typeof Element === 'undefined')
        globalThis.Element = class Element {};
    if (typeof SVGElement === 'undefined')
        globalThis.SVGElement = class SVGElement {};
    if (typeof MathMLElement === 'undefined')
        globalThis.MathMLElement = class MathMLElement {};

    // ---------------------------------------------------------------
    // Event hierarchy — full W3C UIEvents spec coverage
    // ---------------------------------------------------------------

    globalThis.Event = class Event {
        static get NONE() { return 0; }
        static get CAPTURING_PHASE() { return 1; }
        static get AT_TARGET() { return 2; }
        static get BUBBLING_PHASE() { return 3; }
        constructor(type, opts) {
            this.type = type;
            this.bubbles = !!(opts && opts.bubbles);
            this.cancelable = !!(opts && opts.cancelable);
            this.composed = !!(opts && opts.composed);
            this.defaultPrevented = false;
            this.isTrusted = false;
            this.target = null;
            this.currentTarget = null;
            this.eventPhase = 0;
            this.timeStamp = performance.now();
            this._stopped = false;
            this._immediateStopped = false;
            this._composedPath = [];
        }
        get NONE() { return 0; }
        get CAPTURING_PHASE() { return 1; }
        get AT_TARGET() { return 2; }
        get BUBBLING_PHASE() { return 3; }
        preventDefault() {
            if (this.cancelable) {
                this.defaultPrevented = true;
                this._prevented = true;
            }
        }
        stopPropagation() { this._stopped = true; }
        stopImmediatePropagation() { this._stopped = true; this._immediateStopped = true; }
        composedPath() { return this._composedPath || []; }
    };

    globalThis.CustomEvent = class CustomEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.detail = (opts && opts.detail !== undefined) ? opts.detail : null;
        }
    };

    // UIEvent — base for Mouse, Keyboard, Focus, Input, Wheel, Touch events
    globalThis.UIEvent = class UIEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.view = (opts && opts.view) || null;
            this.detail = (opts && opts.detail) || 0;
        }
    };

    globalThis.MouseEvent = class MouseEvent extends UIEvent {
        constructor(type, opts) {
            super(type, opts);
            this.screenX = (opts && opts.screenX) || 0;
            this.screenY = (opts && opts.screenY) || 0;
            this.clientX = (opts && opts.clientX) || 0;
            this.clientY = (opts && opts.clientY) || 0;
            this.pageX = (opts && opts.pageX) || 0;
            this.pageY = (opts && opts.pageY) || 0;
            this.offsetX = (opts && opts.offsetX) || 0;
            this.offsetY = (opts && opts.offsetY) || 0;
            this.movementX = (opts && opts.movementX) || 0;
            this.movementY = (opts && opts.movementY) || 0;
            this.button = (opts && opts.button) || 0;
            this.buttons = (opts && opts.buttons) || 0;
            this.ctrlKey = !!(opts && opts.ctrlKey);
            this.shiftKey = !!(opts && opts.shiftKey);
            this.altKey = !!(opts && opts.altKey);
            this.metaKey = !!(opts && opts.metaKey);
            this.relatedTarget = (opts && opts.relatedTarget) || null;
        }
        getModifierState(key) {
            switch(key) {
                case 'Alt': return this.altKey;
                case 'Control': return this.ctrlKey;
                case 'Shift': return this.shiftKey;
                case 'Meta': return this.metaKey;
                default: return false;
            }
        }
    };

    globalThis.KeyboardEvent = class KeyboardEvent extends UIEvent {
        constructor(type, opts) {
            super(type, opts);
            this.key = (opts && opts.key) || '';
            this.code = (opts && opts.code) || '';
            this.location = (opts && opts.location) || 0;
            this.ctrlKey = !!(opts && opts.ctrlKey);
            this.shiftKey = !!(opts && opts.shiftKey);
            this.altKey = !!(opts && opts.altKey);
            this.metaKey = !!(opts && opts.metaKey);
            this.repeat = !!(opts && opts.repeat);
            this.isComposing = !!(opts && opts.isComposing);
            // Legacy properties
            this.keyCode = (opts && opts.keyCode) || 0;
            this.charCode = (opts && opts.charCode) || 0;
            this.which = (opts && opts.which) || 0;
        }
        getModifierState(key) {
            switch(key) {
                case 'Alt': return this.altKey;
                case 'Control': return this.ctrlKey;
                case 'Shift': return this.shiftKey;
                case 'Meta': return this.metaKey;
                default: return false;
            }
        }
    };

    globalThis.InputEvent = class InputEvent extends UIEvent {
        constructor(type, opts) {
            super(type, opts);
            this.data = (opts && opts.data !== undefined) ? opts.data : null;
            this.inputType = (opts && opts.inputType) || '';
            this.isComposing = !!(opts && opts.isComposing);
            this.dataTransfer = (opts && opts.dataTransfer) || null;
        }
    };

    globalThis.FocusEvent = class FocusEvent extends UIEvent {
        constructor(type, opts) {
            super(type, opts);
            this.relatedTarget = (opts && opts.relatedTarget) || null;
        }
    };

    globalThis.WheelEvent = class WheelEvent extends MouseEvent {
        static get DOM_DELTA_PIXEL() { return 0; }
        static get DOM_DELTA_LINE() { return 1; }
        static get DOM_DELTA_PAGE() { return 2; }
        constructor(type, opts) {
            super(type, opts);
            this.deltaX = (opts && opts.deltaX) || 0;
            this.deltaY = (opts && opts.deltaY) || 0;
            this.deltaZ = (opts && opts.deltaZ) || 0;
            this.deltaMode = (opts && opts.deltaMode) || 0;
        }
        get DOM_DELTA_PIXEL() { return 0; }
        get DOM_DELTA_LINE() { return 1; }
        get DOM_DELTA_PAGE() { return 2; }
    };

    // PointerEvent — extends MouseEvent with pointer-specific properties
    globalThis.PointerEvent = class PointerEvent extends MouseEvent {
        constructor(type, opts) {
            super(type, opts);
            this.pointerId = (opts && opts.pointerId) || 0;
            this.width = (opts && opts.width) || 1;
            this.height = (opts && opts.height) || 1;
            this.pressure = (opts && opts.pressure) || 0;
            this.tangentialPressure = (opts && opts.tangentialPressure) || 0;
            this.tiltX = (opts && opts.tiltX) || 0;
            this.tiltY = (opts && opts.tiltY) || 0;
            this.twist = (opts && opts.twist) || 0;
            this.pointerType = (opts && opts.pointerType) || '';
            this.isPrimary = !!(opts && opts.isPrimary);
        }
    };

    // TouchEvent
    globalThis.Touch = class Touch {
        constructor(opts) {
            this.identifier = (opts && opts.identifier) || 0;
            this.target = (opts && opts.target) || null;
            this.clientX = (opts && opts.clientX) || 0;
            this.clientY = (opts && opts.clientY) || 0;
            this.screenX = (opts && opts.screenX) || 0;
            this.screenY = (opts && opts.screenY) || 0;
            this.pageX = (opts && opts.pageX) || 0;
            this.pageY = (opts && opts.pageY) || 0;
            this.radiusX = (opts && opts.radiusX) || 0;
            this.radiusY = (opts && opts.radiusY) || 0;
            this.rotationAngle = (opts && opts.rotationAngle) || 0;
            this.force = (opts && opts.force) || 0;
        }
    };
    globalThis.TouchList = class TouchList {
        constructor(touches) {
            this._touches = touches || [];
            this.length = this._touches.length;
            for (var i = 0; i < this.length; i++) this[i] = this._touches[i];
        }
        item(i) { return this._touches[i] || null; }
    };
    globalThis.TouchEvent = class TouchEvent extends UIEvent {
        constructor(type, opts) {
            super(type, opts);
            this.touches = (opts && opts.touches) || new TouchList();
            this.targetTouches = (opts && opts.targetTouches) || new TouchList();
            this.changedTouches = (opts && opts.changedTouches) || new TouchList();
            this.ctrlKey = !!(opts && opts.ctrlKey);
            this.shiftKey = !!(opts && opts.shiftKey);
            this.altKey = !!(opts && opts.altKey);
            this.metaKey = !!(opts && opts.metaKey);
        }
    };

    // DragEvent
    globalThis.DragEvent = class DragEvent extends MouseEvent {
        constructor(type, opts) {
            super(type, opts);
            this.dataTransfer = (opts && opts.dataTransfer) || null;
        }
    };

    // CompositionEvent — for IME input
    globalThis.CompositionEvent = class CompositionEvent extends UIEvent {
        constructor(type, opts) {
            super(type, opts);
            this.data = (opts && opts.data) || '';
        }
    };

    // AnimationEvent
    globalThis.AnimationEvent = class AnimationEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.animationName = (opts && opts.animationName) || '';
            this.elapsedTime = (opts && opts.elapsedTime) || 0;
            this.pseudoElement = (opts && opts.pseudoElement) || '';
        }
    };

    // TransitionEvent
    globalThis.TransitionEvent = class TransitionEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.propertyName = (opts && opts.propertyName) || '';
            this.elapsedTime = (opts && opts.elapsedTime) || 0;
            this.pseudoElement = (opts && opts.pseudoElement) || '';
        }
    };

    // HashChangeEvent
    globalThis.HashChangeEvent = class HashChangeEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.oldURL = (opts && opts.oldURL) || '';
            this.newURL = (opts && opts.newURL) || '';
        }
    };

    // PopStateEvent
    globalThis.PopStateEvent = class PopStateEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.state = (opts && opts.state !== undefined) ? opts.state : null;
        }
    };

    // ErrorEvent
    globalThis.ErrorEvent = class ErrorEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.message = (opts && opts.message) || '';
            this.filename = (opts && opts.filename) || '';
            this.lineno = (opts && opts.lineno) || 0;
            this.colno = (opts && opts.colno) || 0;
            this.error = (opts && opts.error) || null;
        }
    };

    // ProgressEvent
    globalThis.ProgressEvent = class ProgressEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.lengthComputable = !!(opts && opts.lengthComputable);
            this.loaded = (opts && opts.loaded) || 0;
            this.total = (opts && opts.total) || 0;
        }
    };

    // ClipboardEvent
    globalThis.ClipboardEvent = class ClipboardEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.clipboardData = (opts && opts.clipboardData) || null;
        }
    };

    // SubmitEvent
    globalThis.SubmitEvent = class SubmitEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.submitter = (opts && opts.submitter) || null;
        }
    };

    // FormDataEvent
    globalThis.FormDataEvent = class FormDataEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.formData = (opts && opts.formData) || null;
        }
    };

    // queueMicrotask — schedules a microtask via Promise
    if (typeof queueMicrotask === 'undefined') {
        globalThis.queueMicrotask = function(cb) {
            Promise.resolve().then(cb);
        };
    }

    // MutationObserver — simplified implementation
    globalThis.MutationObserver = class MutationObserver {
        constructor(callback) {
            this._callback = callback;
            this._targets = [];
            this._records = [];
            this._scheduled = false;
        }
        observe(target, options) {
            this._targets.push({ target, options });
            if (!globalThis.__bro_mutation_observers)
                globalThis.__bro_mutation_observers = [];
            if (!globalThis.__bro_mutation_observers.includes(this))
                globalThis.__bro_mutation_observers.push(this);
        }
        disconnect() {
            this._targets = [];
            if (globalThis.__bro_mutation_observers) {
                var idx = globalThis.__bro_mutation_observers.indexOf(this);
                if (idx >= 0) globalThis.__bro_mutation_observers.splice(idx, 1);
            }
        }
        takeRecords() {
            var r = this._records;
            this._records = [];
            return r;
        }
        _notify(records) {
            this._records = this._records.concat(records);
            if (!this._scheduled) {
                this._scheduled = true;
                var self = this;
                queueMicrotask(function() {
                    self._scheduled = false;
                    var r = self._records;
                    self._records = [];
                    if (r.length > 0) self._callback(r, self);
                });
            }
        }
    };

    // document.activeElement is now a native C++ getter (see js_document_proto_funcs)
})();
)JS";
    JSValue r = JS_Eval(ctx, polyfills, strlen(polyfills),
                        "<dom-polyfills>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, r);

    // ----- Observer polyfills (separate string to stay under MSVC limit) -----
    const char* observerPolyfills = R"JS(
(function() {
    // ---------------------------------------------------------------
    // ResizeObserver — polls element sizes after layout
    // ---------------------------------------------------------------
    globalThis.ResizeObserver = class ResizeObserver {
        constructor(callback) {
            this._callback = callback;
            this._targets = [];
            this._sizes = new Map();
            if (!globalThis.__bro_resize_observers)
                globalThis.__bro_resize_observers = [];
            globalThis.__bro_resize_observers.push(this);
        }
        observe(target, options) {
            if (!this._targets.includes(target))
                this._targets.push(target);
        }
        unobserve(target) {
            var idx = this._targets.indexOf(target);
            if (idx >= 0) this._targets.splice(idx, 1);
            this._sizes.delete(target);
        }
        disconnect() {
            this._targets = [];
            this._sizes.clear();
            if (globalThis.__bro_resize_observers) {
                var idx = globalThis.__bro_resize_observers.indexOf(this);
                if (idx >= 0) globalThis.__bro_resize_observers.splice(idx, 1);
            }
        }
        // Called by engine after layout to check for size changes
        _check() {
            var entries = [];
            for (var i = 0; i < this._targets.length; i++) {
                var target = this._targets[i];
                var rect = target.getBoundingClientRect();
                var prev = this._sizes.get(target);
                var w = rect.width, h = rect.height;
                if (!prev || prev.w !== w || prev.h !== h) {
                    this._sizes.set(target, { w: w, h: h });
                    entries.push({
                        target: target,
                        contentRect: rect,
                        borderBoxSize: [{ inlineSize: w, blockSize: h }],
                        contentBoxSize: [{ inlineSize: w, blockSize: h }],
                        devicePixelContentBoxSize: [{ inlineSize: w, blockSize: h }]
                    });
                }
            }
            if (entries.length > 0)
                this._callback(entries, this);
        }
    };

    // ---------------------------------------------------------------
    // IntersectionObserver — checks element visibility vs viewport
    // ---------------------------------------------------------------
    globalThis.IntersectionObserver = class IntersectionObserver {
        constructor(callback, options) {
            this._callback = callback;
            this._root = (options && options.root) || null;
            this._rootMargin = (options && options.rootMargin) || '0px';
            this._thresholds = (options && options.threshold) || [0];
            if (typeof this._thresholds === 'number')
                this._thresholds = [this._thresholds];
            this._targets = [];
            this._prevRatios = new Map();
            if (!globalThis.__bro_intersection_observers)
                globalThis.__bro_intersection_observers = [];
            globalThis.__bro_intersection_observers.push(this);
        }
        get root() { return this._root; }
        get rootMargin() { return this._rootMargin; }
        get thresholds() { return this._thresholds; }
        observe(target) {
            if (!this._targets.includes(target))
                this._targets.push(target);
        }
        unobserve(target) {
            var idx = this._targets.indexOf(target);
            if (idx >= 0) this._targets.splice(idx, 1);
            this._prevRatios.delete(target);
        }
        disconnect() {
            this._targets = [];
            this._prevRatios.clear();
            if (globalThis.__bro_intersection_observers) {
                var idx = globalThis.__bro_intersection_observers.indexOf(this);
                if (idx >= 0) globalThis.__bro_intersection_observers.splice(idx, 1);
            }
        }
        takeRecords() { return []; }
        // Called by engine after layout
        _check() {
            var vpW = globalThis.innerWidth || 800;
            var vpH = globalThis.innerHeight || 600;
            var rootRect = this._root
                ? this._root.getBoundingClientRect()
                : { x: 0, y: 0, width: vpW, height: vpH, top: 0, left: 0, right: vpW, bottom: vpH };
            var entries = [];
            for (var i = 0; i < this._targets.length; i++) {
                var target = this._targets[i];
                var rect = target.getBoundingClientRect();
                // Calculate intersection
                var intLeft = Math.max(rect.x, rootRect.x);
                var intTop = Math.max(rect.y, rootRect.y);
                var intRight = Math.min(rect.x + rect.width, rootRect.x + rootRect.width);
                var intBottom = Math.min(rect.y + rect.height, rootRect.y + rootRect.height);
                var intW = Math.max(0, intRight - intLeft);
                var intH = Math.max(0, intBottom - intTop);
                var intArea = intW * intH;
                var targetArea = rect.width * rect.height;
                var ratio = targetArea > 0 ? intArea / targetArea : 0;
                var isIntersecting = ratio > 0;
                // Check if we crossed a threshold
                var prevRatio = this._prevRatios.get(target);
                if (prevRatio === undefined) prevRatio = -1;
                var crossed = false;
                for (var t = 0; t < this._thresholds.length; t++) {
                    var th = this._thresholds[t];
                    if ((prevRatio < th && ratio >= th) || (prevRatio >= th && ratio < th)) {
                        crossed = true;
                        break;
                    }
                }
                if (prevRatio === -1) crossed = true; // initial observation
                if (crossed) {
                    this._prevRatios.set(target, ratio);
                    entries.push({
                        target: target,
                        boundingClientRect: rect,
                        intersectionRatio: ratio,
                        intersectionRect: { x: intLeft, y: intTop, width: intW, height: intH,
                            top: intTop, left: intLeft, right: intRight, bottom: intBottom },
                        isIntersecting: isIntersecting,
                        rootBounds: rootRect,
                        time: performance.now()
                    });
                }
            }
            if (entries.length > 0)
                this._callback(entries, this);
        }
    };

})();
)JS";
    JSValue r2 = JS_Eval(ctx, observerPolyfills, strlen(observerPolyfills),
                         "<observer-polyfills>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(ctx, r2);

    // Register native getComputedStyle on window (globalThis)
    {
        JSValue g = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, g, "getComputedStyle",
            JS_NewCFunction(ctx, js_window_getComputedStyle, "getComputedStyle", 1));
        JS_FreeValue(ctx, g);
    }
}

// ===========================================================================
// Cleanup
// ===========================================================================

void DomBindings::cleanup(JSContext* ctx) {
    s_ctx_documents.erase(ctx);
    s_ctx_factories.erase(ctx);
}

void DomBindings::cleanupRuntime(JSRuntime* rt) {
    s_classes_registered.erase(rt);
}

void DomBindings::sweepOrphanedWrappers(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue elemMap = JS_GetPropertyStr(ctx, global, "__bro_elem_map");
    if (JS_IsUndefined(elemMap) || JS_IsNull(elemMap)) {
        JS_FreeValue(ctx, elemMap);
        JS_FreeValue(ctx, global);
        return;
    }

    JSPropertyEnum* props = nullptr;
    uint32_t len = 0;
    JS_GetOwnPropertyNames(ctx, &props, &len, elemMap,
                           JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY);

    for (uint32_t i = 0; i < len; i++) {
        JSValue val = JS_GetProperty(ctx, elemMap, props[i].atom);
        auto* el = static_cast<bro::dom::Element*>(
            JS_GetOpaque(val, js_element_class_id));
        if (el && el->isAlive() && !el->parentNode() &&
            el->childNodes().empty() &&
            el->tagName() == "#DOCUMENT-FRAGMENT") {
            auto* doc = el->document();
            JS_SetOpaque(val, nullptr);
            JS_DeleteProperty(ctx, elemMap, props[i].atom, 0);
            if (doc) doc->freeNode(el);
        }
        JS_FreeValue(ctx, val);
    }

    JS_FreePropertyEnum(ctx, props, len);
    JS_FreeValue(ctx, elemMap);
    JS_FreeValue(ctx, global);
}

} // namespace bro::js
