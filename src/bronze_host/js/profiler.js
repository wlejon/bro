// profiler.js — the public shape of bro.profiler (docs/profiler-api.js), assembled
// over the natives under __bro_native.profiler (native_profiler_decl.h states each C
// entry point, native_profiler_register.cpp registers them, both in natives/profiler/).
// Hand-maintained: a native's signature changes here, in those two files and in
// native_profiler.cpp together.
//
// Every native is spelled by its full dotted path at the point of use: that is the
// spelling the compiler lowers to a direct call.
(function () {
    'use strict';

    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });
    const mount = (root, name) => root[name] !== undefined ? root[name] : (root[name] = {});

    const ns = mount(globalThis.bro, "profiler");
    const THREADS = ['main', 'workers', 'js', 'all'];

    fn(ns, "start", function start(opts) {
        const o = opts === undefined ? {} : opts;
        if (o === null || typeof o !== 'object') throw new TypeError("bro.profiler.start: opts must be an object");
        const hz = o.hz === undefined ? 1000 : o.hz;
        if (typeof hz !== 'number' || !(hz > 0)) throw new TypeError("bro.profiler.start: opts.hz must be a positive number");
        const threads = o.threads === undefined ? 'main' : o.threads;
        if (THREADS.indexOf(threads) < 0) throw new TypeError("bro.profiler.start: opts.threads must be 'main', 'workers', 'js' or 'all'");
        const err = __bro_native.profiler.start(hz, threads);
        if (err !== '') throw new Error(err);
    });

    fn(ns, "stop", function stop(opts) {
        const o = opts === undefined ? {} : opts;
        if (o === null || typeof o !== 'object') throw new TypeError("bro.profiler.stop: opts must be an object");
        const callers = o.callers === true;
        const report = o.report === true;
        const top = o.top === undefined ? 40 : o.top;
        if (typeof top !== 'number' || !(top > 0)) throw new TypeError("bro.profiler.stop: opts.top must be a positive number");
        const json = __bro_native.profiler.stop(callers, report, top | 0);
        if (json === '') throw new Error("bro.profiler.stop: no profile is running");
        return JSON.parse(json);
    });

    Object.defineProperty(ns, "running", {
        get() { return __bro_native.profiler.running(); },
        enumerable: true, configurable: true,
    });

    // profile(fn, opts): start, run fn, stop — the result even when fn throws
    // is not wanted, so a throw stops the profile and rethrows.
    fn(ns, "profile", function profile(body, opts) {
        if (typeof body !== 'function') throw new TypeError("bro.profiler.profile: body must be a function");
        const o = opts === undefined ? {} : opts;
        ns.start(o);
        try {
            body();
        } catch (e) {
            if (ns.running) ns.stop();
            throw e;
        }
        return ns.stop(o);
    });
})();
