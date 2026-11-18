// Symbol.toStringTag for the platform's interfaces, as WebIDL gives every
// interface prototype: Object.prototype.toString(document.body) is
// "[object HTMLBodyElement]", String(gl) "[object WebGLRenderingContext]".
// Libraries branch on it (lodash's type checks, three.js's and pixi's
// ImageBitmap / canvas sniffing, Vue's toRawType).
//
// Runs after every DOM / web global is installed (dom_globals.cpp): each
// global constructor whose prototype does not already carry a tag gets its
// own name as one. The ECMAScript intrinsics are skipped (their tags are
// theirs, and they are frozen). Globals defined as accessors are not read: a
// deferred module (the 3D family, net) enters on its first read, and tagging
// must not enter it. The interface objects that are not reachable as a global
// constructor here (window.location and its kin) are tagged as instances.
(function () {
    'use strict';

    const g = globalThis;
    const tag = Symbol.toStringTag;
    const ecma = new Set([
        'Object', 'Function', 'Array', 'Number', 'Boolean', 'String', 'Symbol', 'BigInt', 'Error', 'EvalError',
        'RangeError', 'ReferenceError', 'SyntaxError', 'TypeError', 'URIError', 'AggregateError', 'SuppressedError',
        'RegExp', 'Date', 'Proxy', 'Promise', 'Map', 'Set', 'WeakMap', 'WeakSet', 'WeakRef', 'FinalizationRegistry',
        'ArrayBuffer', 'SharedArrayBuffer', 'DataView', 'Int8Array', 'Uint8Array', 'Uint8ClampedArray', 'Int16Array',
        'Uint16Array', 'Int32Array', 'Uint32Array', 'Float16Array', 'Float32Array', 'Float64Array', 'BigInt64Array',
        'BigUint64Array', 'Iterator', 'AsyncIterator', 'DisposableStack', 'AsyncDisposableStack',
    ]);

    function define(obj, name) {
        if (!obj || (typeof obj !== 'object' && typeof obj !== 'function')) return;
        if (Object.prototype.hasOwnProperty.call(obj, tag) || !Object.isExtensible(obj)) return;
        try {
            Object.defineProperty(obj, tag, { value: name, writable: false, enumerable: false, configurable: true });
        } catch (e) { /* a non-configurable host object: leave it */ }
    }

    // DOMParser and XMLSerializer are native functions returning a plain
    // object of methods (host_parser.cpp): `new` makes it an instance of the
    // interface, so instanceof and the tag hold.
    for (const name of ['DOMParser', 'XMLSerializer']) {
        const Native = g[name];
        if (typeof Native !== 'function') continue;
        const Iface = function () {
            if (!new.target) throw new TypeError("Failed to construct '" + name + "': Please use the 'new' operator.");
            return Object.setPrototypeOf(Native(), new.target.prototype);
        };
        Object.defineProperty(Iface, 'name', { value: name });
        g[name] = Iface;
    }

    for (const name of Object.getOwnPropertyNames(g)) {
        if (ecma.has(name) || !/^[A-Z]/.test(name)) continue;
        const d = Object.getOwnPropertyDescriptor(g, name);
        if (!d || !('value' in d) || typeof d.value !== 'function') continue;
        const proto = d.value.prototype;
        if (!proto || typeof proto !== 'object') continue;
        define(proto, name);
    }

    const instances = [
        ['document', 'HTMLDocument'], ['location', 'Location'], ['navigator', 'Navigator'], ['history', 'History'],
        ['screen', 'Screen'], ['performance', 'Performance'], ['localStorage', 'Storage'],
        ['sessionStorage', 'Storage'], ['crypto', 'Crypto'], ['visualViewport', 'VisualViewport'],
    ];
    for (const [name, iface] of instances) {
        const d = Object.getOwnPropertyDescriptor(g, name);
        let v;
        if (d && 'value' in d) v = d.value;
        else if (d && d.get) { try { v = d.get.call(g); } catch (e) { v = undefined; } }
        if (v && typeof v === 'object' && Object.prototype.toString.call(v) !== '[object ' + iface + ']') define(v, iface);
    }
    if (g.document && g.document.fonts) define(g.document.fonts, 'FontFaceSet');
    if (g.window && g.window !== g) define(g.window, 'Window');
})();
