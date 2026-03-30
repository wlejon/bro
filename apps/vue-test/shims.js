// Minimal browser API shims for Vue 3 compatibility
if (typeof SVGElement === 'undefined') {
    globalThis.SVGElement = class SVGElement {};
}
if (typeof MathMLElement === 'undefined') {
    globalThis.MathMLElement = class MathMLElement {};
}
if (typeof HTMLElement === 'undefined') {
    globalThis.HTMLElement = class HTMLElement {};
}
if (typeof customElements === 'undefined') {
    globalThis.customElements = undefined;
}
if (typeof Event === 'undefined') {
    globalThis.Event = class Event {
        constructor(type, opts) {
            this.type = type;
            this.bubbles = opts?.bubbles || false;
            this.cancelable = opts?.cancelable || false;
        }
    };
}
if (typeof CustomEvent === 'undefined') {
    globalThis.CustomEvent = class CustomEvent extends Event {
        constructor(type, opts) {
            super(type, opts);
            this.detail = opts?.detail || null;
        }
    };
}
console.log('Shims loaded');
