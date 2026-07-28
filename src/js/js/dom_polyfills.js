(function() {
    // document.implementation.createHTMLDocument
    document.implementation = {
        createHTMLDocument: function(title) {
            var head = document.createElement('head');
            var body = document.createElement('body');
            var html = document.createElement('html');
            html.appendChild(head);
            html.appendChild(body);
            return {
                nodeType: 9,
                documentElement: html,
                head: head,
                body: body,
                createElement: function(tag) { return document.createElement(tag); },
                createElementNS: function(ns, tag) { return document.createElementNS(ns, tag); },
                createTextNode: function(text) { return document.createTextNode(text); },
                createDocumentFragment: function() { return document.createDocumentFragment(); }
            };
        }
    };

    // document.location alias (jQuery reads this)
    if (typeof document.location === 'undefined' && typeof window !== 'undefined')
        try { document.location = window.location; } catch(e) {}

    // document.createTreeWalker — brokit ships TreeWalker but leaves the
    // document hookup to the consumer (it has no DOM of its own).
    if (typeof document.createTreeWalker !== 'function' &&
        typeof globalThis.__brokit_install_createTreeWalker === 'function') {
        globalThis.__brokit_install_createTreeWalker(document);
    }

    // Array.from polyfill (QuickJS may not have it)
    if (!Array.from) {
        Array.from = function(obj, mapFn) {
            var arr = [];
            for (var i = 0; i < obj.length; i++) arr.push(mapFn ? mapFn(obj[i], i) : obj[i]);
            return arr;
        };
    }

    // NodeList.prototype.forEach — NodeList is registered as a non-global
    // class in bro, so grab its prototype via an instance.
    try {
        var _nl = document.querySelectorAll('*');
        var _nlProto = _nl && Object.getPrototypeOf(_nl);
        if (_nlProto && !_nlProto.forEach) {
            _nlProto.forEach = function(cb, thisArg) {
                for (var i = 0, n = this.length; i < n; i++) cb.call(thisArg, this[i], i, this);
            };
        }
        if (_nlProto && !_nlProto[Symbol.iterator]) {
            _nlProto[Symbol.iterator] = function() {
                var self = this, i = 0;
                return { next: function() {
                    return i < self.length ? { value: self[i++], done: false } : { value: undefined, done: true };
                }};
            };
        }
    } catch(e) {}

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

    // MutationObserver — full implementation
    globalThis.MutationObserver = class MutationObserver {
        constructor(callback) {
            if (typeof callback !== 'function')
                throw new TypeError('MutationObserver requires a callback function');
            this._callback = callback;
            this._targets = [];
            this._records = [];
            this._scheduled = false;
        }
        observe(target, options) {
            if (!options) options = {};
            // Per spec: if none of the three are set, and attributeFilter is present,
            // default attributes to true
            if (!options.childList && !options.attributes && !options.characterData) {
                if (options.attributeFilter) {
                    options.attributes = true;
                } else if (!options.attributeOldValue && !options.characterDataOldValue) {
                    throw new TypeError('MutationObserver.observe: at least one of childList, attributes, or characterData must be true');
                }
            }
            if (options.attributeOldValue) options.attributes = true;
            if (options.characterDataOldValue) options.characterData = true;
            // Remove any existing observation of this target
            this._targets = this._targets.filter(function(e) { return e.target !== target; });
            this._targets.push({ target: target, options: options });
            if (!globalThis.__bro_mutation_observers)
                globalThis.__bro_mutation_observers = [];
            if (!globalThis.__bro_mutation_observers.includes(this))
                globalThis.__bro_mutation_observers.push(this);
        }
        unobserve(target) {
            this._targets = this._targets.filter(function(e) { return e.target !== target; });
            if (this._targets.length === 0) this.disconnect();
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

    // ---- Page visibility + fullscreen state ----------------------------------
    // The engine drives these via __bro_set_visibility / __bro_set_fullscreen
    // when SDL window focus changes or the graphics.fullscreen setting flips.
    document.hidden = false;
    document.visibilityState = 'visible';
    document.fullscreenElement = null;
    document.fullscreenEnabled = true;

    function _fireDocEvent(type) {
        var evt;
        if (typeof CustomEvent === 'function') {
            evt = new CustomEvent(type, { bubbles: false, cancelable: false });
            try { evt.isTrusted = true; } catch(_) {}
        } else {
            evt = { type: type, target: document, bubbles: false,
                    cancelable: false, defaultPrevented: false, isTrusted: true,
                    preventDefault: function() { this.defaultPrevented = true; } };
        }
        // Fires the document's own listeners (which live on documentElement)
        // and, on the way out, window's. The explicit window call below is
        // still needed because these events do not bubble.
        if (typeof document.dispatchEvent === 'function') {
            try { document.dispatchEvent(evt); } catch(_) {}
        }
        if (typeof globalThis.__bro_dispatch_window_event === 'function') {
            globalThis.__bro_dispatch_window_event(type, evt);
        }
    }

    globalThis.__bro_set_visibility = function(visible) {
        var next = visible ? 'visible' : 'hidden';
        if (document.visibilityState === next) return;
        document.visibilityState = next;
        document.hidden = !visible;
        _fireDocEvent('visibilitychange');
    };
    globalThis.__bro_set_fullscreen = function(on) {
        var cur = document.fullscreenElement;
        var next = on ? (document.documentElement || document.body) : null;
        if (cur === next) return;
        document.fullscreenElement = next;
        _fireDocEvent('fullscreenchange');
    };

    if (typeof document.exitFullscreen === 'undefined' &&
        typeof globalThis.__bro_set_fullscreen_setting === 'function') {
        document.exitFullscreen = function() {
            return globalThis.__bro_set_fullscreen_setting(false);
        };
    }

    // ---- Element scroll helpers / requestFullscreen --------------------------
    // qjsbind registers Element instances on a prototype that isn't the same
    // object as globalThis.Element.prototype. Reach the real proto via a
    // temporary element and install methods there. Methods guard themselves
    // with `typeof === undefined` so re-runs are safe.
    (function() {
        var tmp = document.createElement('div');
        var elProto = Object.getPrototypeOf(tmp);
        if (!elProto) return;

        var scrollArgs = globalThis.__bro_scroll_args ||
            function(a, b) {
                if (a !== null && typeof a === 'object' && !Array.isArray(a)) return { x: a.left, y: a.top };
                return { x: a, y: b };
            };
        if (typeof elProto.scrollTo !== 'function') {
            elProto.scrollTo = function(a, b) {
                var p = scrollArgs(a, b);
                if (typeof p.x === 'number') this.scrollLeft = p.x;
                if (typeof p.y === 'number') this.scrollTop  = p.y;
            };
        }
        if (typeof elProto.scrollBy !== 'function') {
            elProto.scrollBy = function(a, b) {
                var p = scrollArgs(a, b);
                if (typeof p.x === 'number') this.scrollLeft = this.scrollLeft + p.x;
                if (typeof p.y === 'number') this.scrollTop  = this.scrollTop  + p.y;
            };
        }
        if (typeof elProto.requestFullscreen !== 'function' &&
            typeof globalThis.__bro_set_fullscreen_setting === 'function') {
            elProto.requestFullscreen = function() {
                return globalThis.__bro_set_fullscreen_setting(true);
            };
        }

        // ---- AbortSignal support in addEventListener -------------------------
        // Wrap addEventListener so {signal} removes the listener when aborted.
        // Applies to Element, document (which forwards to documentElement), and
        // window (whose addEventListener lives on globalThis).
        function wrapWithSignal(proto) {
            var orig = proto.addEventListener;
            if (!orig || orig.__bro_signal_wrapped) return;
            proto.addEventListener = function(type, fn, opts) {
                var signal = (opts && typeof opts === 'object') ? opts.signal : null;
                if (signal && signal.aborted) return;
                orig.call(this, type, fn, opts);
                if (signal && typeof signal.addEventListener === 'function') {
                    var self = this;
                    signal.addEventListener('abort', function onAbort() {
                        self.removeEventListener(type, fn, opts);
                    });
                }
            };
            proto.addEventListener.__bro_signal_wrapped = true;
        }
        wrapWithSignal(elProto);
        // window: addEventListener is defined directly on globalThis.
        wrapWithSignal(globalThis);
    })();

    // ---- XMLSerializer -------------------------------------------------------
    // (DOMParser is native — see document_bindings.cpp: parseFromString
    // returns a real detached Document parsed by the engine's HTML parser.)
    //
    // XMLSerializer: serialize a node back to a string. For elements, we
    // delegate to outerHTML. Documents round-trip via their documentElement.
    if (typeof globalThis.XMLSerializer === 'undefined') {
        function XMLSerializer() {}
        XMLSerializer.prototype.serializeToString = function(node) {
            if (!node) return '';
            if (typeof node.outerHTML === 'string') return node.outerHTML;
            // Document-shaped input
            if (node.nodeType === 9 && node.documentElement) {
                return node.documentElement.outerHTML || '';
            }
            if (typeof node.textContent === 'string') return node.textContent;
            return String(node);
        };
        globalThis.XMLSerializer = XMLSerializer;
    }

    // window.getSelection → document.getSelection. Both return the same
    // Selection singleton per spec; the native method lives on document, so
    // the window alias just forwards. Runs after DomBindings::install so
    // document.getSelection is already bound.
    if (typeof document !== 'undefined'
            && typeof document.getSelection === 'function'
            && typeof globalThis.getSelection !== 'function') {
        globalThis.getSelection = function() { return document.getSelection(); };
    }
})();
