// models.js — the public shape of bro.models (docs/models-api.js), assembled over the
// natives under __bro_native.models (native_models_decl.h states each C entry point,
// native_models_register.cpp registers them, both in natives/models/; the bodies are
// native_models.cpp's). Hand-maintained: a native's signature changes in all four together.
//
// Every native is spelled by its full dotted path at the point of use: that is the
// spelling the compiler lowers to a direct call. The roots this module reads as bare
// identifiers are listed in module.globals beside it.
(function () {
    'use strict';

    const accessor = (obj, name, get, set) =>
        Object.defineProperty(obj, name, { get, set, enumerable: true, configurable: true });
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });
    const mount = (root, name) => root[name] !== undefined ? root[name] : (root[name] = {});

    const ns = mount(globalThis.bro, 'models');

    // A fresh array each read: the list follows BRO_MODELS and the setting as they are now.
    accessor(ns, 'dirs', function () {
        const joined = __bro_native.models.dirs();
        return joined === '' ? [] : joined.split('\n');
    }, undefined);

    fn(ns, 'find', function find(name) {
        if (typeof name !== 'string' || name === '') {
            throw new TypeError('bro.models.find: name must be a non-empty string');
        }
        const segs = name.split(/[\\/]/);
        if (/^([\\/]|[A-Za-z]:)/.test(name) || segs.indexOf('..') >= 0) {
            throw new TypeError('bro.models.find: name must be a path relative to a models directory, without ".."');
        }
        const dir = __bro_native.models.find(name);
        return dir === '' ? null : dir;
    });
})();
