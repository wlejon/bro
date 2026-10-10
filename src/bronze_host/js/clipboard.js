// The async Clipboard API's item half: `ClipboardItem`, and
// `navigator.clipboard.write(items)` / `read()` over it. readText/writeText
// are host_navigator.cpp's own; this module adds the two that carry more than
// one representation, built on the synchronous primitives that file puts on
// the same object:
//
//   __writeItems(types, buffers)  replace the clipboard with every
//                                 representation of one item at once
//                                 (platform::Clipboard::setData); false when
//                                 the OS refused the write
//   __readItems()                 [type, ArrayBuffer, type, ArrayBuffer, ...]:
//                                 "text/plain" when there is text, "image/png"
//                                 when there is an image (any format the OS
//                                 holds it in, re-encoded as PNG)
//
// Types are what Chromium supports on the web: "text/plain" and "image/png".
// One item per write, as in every browser; a second is NotAllowedError.
// See docs/clipboard-api.js.
//
// HOW IT IS SHIPPED. Compiled by bronze at build time (bro_compile_js in
// ../CMakeLists.txt, against js/module.globals) and entered from
// installClipboardModule() (host_js_modules.cpp) after brokit's installers,
// so `Blob` and `DOMException` are on globalThis; the installer lifts
// `ClipboardItem` into the host-global registry.
(function () {
    'use strict';

    const g = globalThis;
    const nav = g.navigator;
    const clip = nav ? nav.clipboard : undefined;
    if (!clip || typeof clip.__writeItems !== 'function') return;

    const SUPPORTED = ['text/plain', 'image/png'];

    function domError(name, message) {
        const D = g.DOMException;
        if (typeof D === 'function') return new D(message, name);
        const e = new Error(message);
        e.name = name;
        return e;
    }

    function isBlob(v) {
        return typeof g.Blob === 'function' && v instanceof g.Blob;
    }

    // What a representation resolves to, as a Blob of `type`.
    function toBlob(value, type) {
        if (isBlob(value)) return value;
        if (typeof value === 'string') return new g.Blob([value], { type: type });
        throw new TypeError("ClipboardItem: the value for '" + type +
                            "' is not a Blob or a string");
    }

    const state = new WeakMap();  // item -> { map: Map(type -> value), types, style }

    class ClipboardItem {
        constructor(items, options) {
            if (items === null || typeof items !== 'object') {
                throw new TypeError("Failed to construct 'ClipboardItem': " +
                                    'the items argument must be an object');
            }
            const keys = Object.keys(items);
            if (keys.length === 0) {
                throw new TypeError("Failed to construct 'ClipboardItem': " +
                                    'Empty dictionary argument');
            }
            const map = new Map();
            for (const k of keys) map.set(k, items[k]);
            let style = 'unspecified';
            if (options && (options.presentationStyle === 'inline' ||
                            options.presentationStyle === 'attachment')) {
                style = options.presentationStyle;
            }
            state.set(this, { map: map, types: Object.freeze(keys.slice()), style: style });
        }

        get types() {
            const s = state.get(this);
            return s ? s.types : Object.freeze([]);
        }

        get presentationStyle() {
            const s = state.get(this);
            return s ? s.style : 'unspecified';
        }

        getType(type) {
            const s = state.get(this);
            const t = String(type);
            if (!s || !s.map.has(t)) {
                return Promise.reject(domError('NotFoundError',
                    "Failed to execute 'getType' on 'ClipboardItem': The type '" +
                    t + "' was not found"));
            }
            return Promise.resolve(s.map.get(t)).then((v) => toBlob(v, t));
        }

        static supports(type) {
            return SUPPORTED.indexOf(String(type)) !== -1;
        }
    }
    Object.defineProperty(ClipboardItem.prototype, Symbol.toStringTag, {
        value: 'ClipboardItem', configurable: true,
    });

    const PNG_SIGNATURE = [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A];
    function isPng(buffer) {
        const b = new Uint8Array(buffer);
        if (b.length < 8) return false;
        for (let i = 0; i < 8; ++i) if (b[i] !== PNG_SIGNATURE[i]) return false;
        return true;
    }

    // Plain promise chains rather than an async function: the representations
    // are gathered in order, then written in one go.
    function write(data) {
        if (data === null || data === undefined || typeof data[Symbol.iterator] !== 'function') {
            return Promise.reject(new TypeError("Failed to execute 'write' on 'Clipboard': " +
                                                'the argument is not a sequence of ClipboardItem'));
        }
        const list = Array.from(data);
        if (list.length > 1) {
            return Promise.reject(domError('NotAllowedError',
                'Support for multiple ClipboardItems is not implemented.'));
        }
        const types = [];
        const buffers = [];
        let chain = Promise.resolve();
        if (list.length === 1) {
            const item = list[0];
            if (!(item instanceof ClipboardItem)) {
                return Promise.reject(new TypeError("Failed to execute 'write' on 'Clipboard': " +
                                                    'the item is not a ClipboardItem'));
            }
            for (const t of item.types) {
                if (!ClipboardItem.supports(t)) {
                    return Promise.reject(domError('NotAllowedError',
                        'Type ' + t + ' not supported on write.'));
                }
            }
            for (const t of item.types) {
                chain = chain
                    .then(() => item.getType(t))
                    .then((blob) => blob.arrayBuffer())
                    .then((buffer) => {
                        if (t === 'image/png' && !isPng(buffer)) {
                            throw domError('DataError', 'The image/png representation is not a PNG.');
                        }
                        types.push(t);
                        buffers.push(buffer);
                    });
            }
        }
        return chain.then(() => {
            if (!clip.__writeItems(types, buffers)) {
                throw domError('NotAllowedError', 'clipboard write failed');
            }
        });
    }

    function read() {
        return new Promise((resolve) => {
            const flat = clip.__readItems();
            const items = {};
            let n = 0;
            for (let i = 0; i + 1 < flat.length; i += 2) {
                items[flat[i]] = new g.Blob([flat[i + 1]], { type: flat[i] });
                ++n;
            }
            resolve(n ? [new ClipboardItem(items)] : []);
        });
    }

    for (const [name, fn] of [['write', write], ['read', read]]) {
        Object.defineProperty(clip, name, {
            value: fn, writable: true, enumerable: true, configurable: true,
        });
    }
    g.ClipboardItem = ClipboardItem;
})();
