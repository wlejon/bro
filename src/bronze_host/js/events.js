// The UI event CLASSES a page constructs: UIEvent, MouseEvent, KeyboardEvent,
// InputEvent, FocusEvent, WheelEvent, PointerEvent, DragEvent,
// CompositionEvent, AnimationEvent, TransitionEvent, ClipboardEvent,
// SubmitEvent, ErrorEvent, ProgressEvent, PromiseRejectionEvent — over the
// `Event` brokit installs (event_target.js), so `new MouseEvent('click')
// instanceof Event` holds and the base's preventDefault / stopPropagation /
// composedPath are inherited rather than copied.
//
// These are the INITIALIZER shapes W3C UI Events specifies: each constructor
// reads its dictionary members off `opts` with the spec's defaults and puts
// them on the instance as plain data properties. That is what a program
// needs from `new KeyboardEvent('keydown', {key: 'a'})` — an object it can
// hand to `dispatchEvent`, whose reader takes `type`, `bubbles`,
// `cancelable`, `detail`, `key` and `code` off it (host_dom_events.cpp) and
// builds the engine's own dom::Event for the walk. The event a LISTENER
// receives is not one of these: it is the per-dispatch copy that file
// describes, born from the engine's event, which is why nothing here has to
// agree with the engine about a field the engine did not send.
//
// HOW IT IS SHIPPED. Compiled by bronze at build time (bro_compile_js in
// ../CMakeLists.txt, against js/module.globals) and entered from
// installEventsModule() (host_js_modules.cpp) AFTER brokit's installers, so
// `globalThis.Event` is there to extend; the installer then lifts each class
// into the host-global registry so a compiled bare `MouseEvent` resolves.
(function () {
    'use strict';

    const g = globalThis;
    const Event = g.Event;
    if (typeof Event !== 'function') {
        throw new TypeError('events.js: globalThis.Event is not installed; ' +
                            'brokit\'s installEventTarget must run first');
    }

    const num = (opts, k, d) => (opts && typeof opts[k] === 'number') ? opts[k] : d;
    const str = (opts, k, d) => (opts && typeof opts[k] === 'string') ? opts[k] : d;
    const flag = (opts, k) => !!(opts && opts[k]);
    const ref = (opts, k) => (opts && opts[k] !== undefined) ? opts[k] : null;

    function modifierState(key) {
        switch (key) {
            case 'Alt': return this.altKey;
            case 'Control': return this.ctrlKey;
            case 'Shift': return this.shiftKey;
            case 'Meta': return this.metaKey;
            default: return false;
        }
    }

    class UIEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.view = ref(opts, 'view');
            this.detail = num(opts, 'detail', 0);
        }
    }

    class MouseEvent extends UIEvent {
        constructor(type, opts) {
            super(type, opts);
            this.screenX = num(opts, 'screenX', 0);
            this.screenY = num(opts, 'screenY', 0);
            this.clientX = num(opts, 'clientX', 0);
            this.clientY = num(opts, 'clientY', 0);
            this.pageX = num(opts, 'pageX', this.clientX);
            this.pageY = num(opts, 'pageY', this.clientY);
            this.offsetX = num(opts, 'offsetX', 0);
            this.offsetY = num(opts, 'offsetY', 0);
            this.x = this.clientX;
            this.y = this.clientY;
            this.movementX = num(opts, 'movementX', 0);
            this.movementY = num(opts, 'movementY', 0);
            this.button = num(opts, 'button', 0);
            this.buttons = num(opts, 'buttons', 0);
            this.ctrlKey = flag(opts, 'ctrlKey');
            this.shiftKey = flag(opts, 'shiftKey');
            this.altKey = flag(opts, 'altKey');
            this.metaKey = flag(opts, 'metaKey');
            this.relatedTarget = ref(opts, 'relatedTarget');
        }
    }
    MouseEvent.prototype.getModifierState = modifierState;

    class KeyboardEvent extends UIEvent {
        constructor(type, opts) {
            super(type, opts);
            this.key = str(opts, 'key', '');
            this.code = str(opts, 'code', '');
            this.location = num(opts, 'location', 0);
            this.ctrlKey = flag(opts, 'ctrlKey');
            this.shiftKey = flag(opts, 'shiftKey');
            this.altKey = flag(opts, 'altKey');
            this.metaKey = flag(opts, 'metaKey');
            this.repeat = flag(opts, 'repeat');
            this.isComposing = flag(opts, 'isComposing');
            // Legacy members, still read by code that predates `key`.
            this.keyCode = num(opts, 'keyCode', 0);
            this.charCode = num(opts, 'charCode', 0);
            this.which = num(opts, 'which', this.keyCode);
        }
    }
    KeyboardEvent.prototype.getModifierState = modifierState;
    KeyboardEvent.DOM_KEY_LOCATION_STANDARD = 0;
    KeyboardEvent.DOM_KEY_LOCATION_LEFT = 1;
    KeyboardEvent.DOM_KEY_LOCATION_RIGHT = 2;
    KeyboardEvent.DOM_KEY_LOCATION_NUMPAD = 3;

    class InputEvent extends UIEvent {
        constructor(type, opts) {
            super(type, opts);
            this.data = (opts && opts.data !== undefined) ? opts.data : null;
            this.inputType = str(opts, 'inputType', '');
            this.isComposing = flag(opts, 'isComposing');
            this.dataTransfer = ref(opts, 'dataTransfer');
        }
    }

    class FocusEvent extends UIEvent {
        constructor(type, opts) {
            super(type, opts);
            this.relatedTarget = ref(opts, 'relatedTarget');
        }
    }

    class WheelEvent extends MouseEvent {
        constructor(type, opts) {
            super(type, opts);
            this.deltaX = num(opts, 'deltaX', 0);
            this.deltaY = num(opts, 'deltaY', 0);
            this.deltaZ = num(opts, 'deltaZ', 0);
            this.deltaMode = num(opts, 'deltaMode', 0);
        }
    }
    WheelEvent.DOM_DELTA_PIXEL = 0;
    WheelEvent.DOM_DELTA_LINE = 1;
    WheelEvent.DOM_DELTA_PAGE = 2;

    class PointerEvent extends MouseEvent {
        constructor(type, opts) {
            super(type, opts);
            this.pointerId = num(opts, 'pointerId', 0);
            this.width = num(opts, 'width', 1);
            this.height = num(opts, 'height', 1);
            this.pressure = num(opts, 'pressure', 0);
            this.tangentialPressure = num(opts, 'tangentialPressure', 0);
            this.tiltX = num(opts, 'tiltX', 0);
            this.tiltY = num(opts, 'tiltY', 0);
            this.twist = num(opts, 'twist', 0);
            this.pointerType = str(opts, 'pointerType', '');
            this.isPrimary = flag(opts, 'isPrimary');
        }
        getCoalescedEvents() { return [this]; }
        getPredictedEvents() { return []; }
    }

    class DragEvent extends MouseEvent {
        constructor(type, opts) {
            super(type, opts);
            this.dataTransfer = ref(opts, 'dataTransfer');
        }
    }

    class CompositionEvent extends UIEvent {
        constructor(type, opts) {
            super(type, opts);
            this.data = str(opts, 'data', '');
        }
    }

    class AnimationEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.animationName = str(opts, 'animationName', '');
            this.elapsedTime = num(opts, 'elapsedTime', 0);
            this.pseudoElement = str(opts, 'pseudoElement', '');
        }
    }

    class TransitionEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.propertyName = str(opts, 'propertyName', '');
            this.elapsedTime = num(opts, 'elapsedTime', 0);
            this.pseudoElement = str(opts, 'pseudoElement', '');
        }
    }

    class ClipboardEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.clipboardData = ref(opts, 'clipboardData');
        }
    }

    class SubmitEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.submitter = ref(opts, 'submitter');
        }
    }

    class ErrorEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.message = str(opts, 'message', '');
            this.filename = str(opts, 'filename', '');
            this.lineno = num(opts, 'lineno', 0);
            this.colno = num(opts, 'colno', 0);
            this.error = (opts && opts.error !== undefined) ? opts.error : null;
        }
    }

    class ProgressEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.lengthComputable = flag(opts, 'lengthComputable');
            this.loaded = num(opts, 'loaded', 0);
            this.total = num(opts, 'total', 0);
        }
    }

    class PromiseRejectionEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            if (!opts || !('promise' in opts)) {
                throw new TypeError('PromiseRejectionEvent: the `promise` member is required');
            }
            this.promise = opts.promise;
            this.reason = (opts.reason !== undefined) ? opts.reason : undefined;
        }
    }

    g.UIEvent = UIEvent;
    g.MouseEvent = MouseEvent;
    g.KeyboardEvent = KeyboardEvent;
    g.InputEvent = InputEvent;
    g.FocusEvent = FocusEvent;
    g.WheelEvent = WheelEvent;
    g.PointerEvent = PointerEvent;
    g.DragEvent = DragEvent;
    g.CompositionEvent = CompositionEvent;
    g.AnimationEvent = AnimationEvent;
    g.TransitionEvent = TransitionEvent;
    g.ClipboardEvent = ClipboardEvent;
    g.SubmitEvent = SubmitEvent;
    g.ErrorEvent = ErrorEvent;
    g.ProgressEvent = ProgressEvent;
    g.PromiseRejectionEvent = PromiseRejectionEvent;
})();
