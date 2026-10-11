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

    accessor(ns, 'sources', function () {
        const joined = __bro_native.models.sources();
        return joined === '' ? [] : joined.split('\n');
    }, undefined);

    const checkName = (what, name) => {
        if (typeof name !== 'string' || name === '') {
            throw new TypeError(`bro.models.${what}: name must be a non-empty string`);
        }
        const segs = name.split(/[\\/]/);
        if (/^([\\/]|[A-Za-z]:)/.test(name) || segs.indexOf('..') >= 0) {
            throw new TypeError(`bro.models.${what}: name must be a path relative to a models directory, without ".."`);
        }
    };

    const failure = (what, s) => {
        const e = new Error(`bro.models.${what}: ${s.error || 'failed'}`);
        e.code = s.code || 'network';
        return e;
    };

    const abortError = (signal) => {
        if (signal && signal.reason !== undefined) return signal.reason;
        const D = globalThis.DOMException;
        if (typeof D === 'function') return new D('The fetch was aborted', 'AbortError');
        const e = new Error('The fetch was aborted');
        e.name = 'AbortError';
        e.code = 'aborted';
        return e;
    };

    // Runs one native job to its end: polls it from the event loop (the job is
    // on its own thread), reports progress, settles with settle(snapshot).
    const runJob = (what, name, listOnly, opts, settle) => new Promise((resolve, reject) => {
        const onProgress = opts && typeof opts.onProgress === 'function' ? opts.onProgress : null;
        const signal = opts ? opts.signal : undefined;
        if (signal && signal.aborted) { reject(abortError(signal)); return; }
        const id = __bro_native.models.fetchStart(name, listOnly);
        let finished = false;
        let lastDone = -1, lastTotal = -1;
        const onAbort = () => {
            if (finished) return;
            finished = true;
            __bro_native.models.fetchCancel(id);
            __bro_native.models.fetchPoll(id);  // forgets this caller's id
            reject(abortError(signal));
        };
        if (signal && typeof signal.addEventListener === 'function') signal.addEventListener('abort', onAbort);
        const done = (fn, v) => {
            finished = true;
            if (signal && typeof signal.removeEventListener === 'function') signal.removeEventListener('abort', onAbort);
            fn(v);
        };
        const tick = () => {
            if (finished) return;
            const s = JSON.parse(__bro_native.models.fetchPoll(id));
            if (onProgress && (s.done !== lastDone || s.total !== lastTotal) && (s.total > 0 || s.state !== 'running')) {
                lastDone = s.done; lastTotal = s.total;
                try {
                    onProgress({ done: s.done, total: s.total, file: s.file, files: s.files, source: s.source });
                } catch (e) {
                    if (s.state === 'running') {
                        __bro_native.models.fetchCancel(id);
                        __bro_native.models.fetchPoll(id);
                    }
                    done(reject, e);
                    return;
                }
            }
            if (s.state === 'running') { globalThis.setTimeout(tick, 50); return; }
            if (s.state === 'cancelled') { done(reject, abortError(signal)); return; }
            try {
                done(resolve, settle(s));
            } catch (e) {
                done(reject, e);
            }
        };
        tick();
    });

    fn(ns, 'fetch', function fetch(name, opts) {
        try { checkName('fetch', name); } catch (e) { return Promise.reject(e); }
        const have = __bro_native.models.find(name);
        if (have !== '') return Promise.resolve(have);
        return runJob('fetch', name, false, opts, (s) => {
            if (s.state !== 'done') throw failure('fetch', s);
            return s.dir;
        });
    });

    fn(ns, 'available', function available(name, opts) {
        try { checkName('available', name); } catch (e) { return Promise.reject(e); }
        return runJob('available', name, true, opts, (s) => {
            if (s.state === 'done') return { source: s.source, bytes: s.total, files: s.files };
            if (s.code === 'not-found') return null;
            throw failure('available', s);
        });
    });
})();
