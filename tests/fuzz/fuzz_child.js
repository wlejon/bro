// One namespace of the native fuzz (test_native_fuzz.js runs this once per
// namespace, each in a bro-headless of its own, as the capability-less app
// tests/test_app/fuzz_bare):
//
//   bro-headless tests/test_app/fuzz_bare tests/fuzz/fuzz_child.js -- <ns> <last-call-file>
//
// Every function reachable from bro.<ns> (namespace members, a class's
// statics, its instances' methods, and those methods again after the
// instance is disposed) is called with arguments a native must refuse
// rather than crash on. Before each call its name and argument set go to
// <last-call-file>, so a child that dies or hangs names the call that did it.
// Throwing is the right answer to all of them; only the process dying is a
// finding.

const [ns, lastFile, traceArg] = bro.app.argv;
// `trace` as a third argument prints every call, for finding which earlier
// calls left a handle in the state a later one fails on.
const trace = traceArg === 'trace';

const VALUES = [
    ['undefined', undefined], ['null', null], ['NaN', NaN], ['-1', -1], ['0', 0],
    ['1.5', 1.5], ['1e308', 1e308], ['-1e308', -1e308], ['2^31', 2 ** 31], ['2^53', 2 ** 53],
    ['Infinity', Infinity], ["''", ''], ["'x'", 'x'], ['{}', {}], ['[]', []],
    ['[NaN,-1]', [NaN, -1]], ['f32[0]', new Float32Array(0)], ['u8[3]', new Uint8Array(3)],
    ['fn', () => {}], ['true', true], ['symbol', Symbol('fuzz')], ['1n', 1n],
];

// What would end the run, block it, or act on the session rather than fail
// on bad input; what a bare app's grants refuse anyway; and work as long as
// the count asked for (flora's grow(world, 2^31) is 2^31 ticks).
const SKIP = /^(exit|quit|reload|sleep|wallSleep|advanceTime|runFrames|spawn|launch|kill|close|stop|terminate|wait|waitFor|block|join|serve|listen|host|connect|requestPermission|grow)$/;

function argSets(fn) {
    const n = Math.max(1, Math.min(fn.length || 1, 6));
    const sets = [['()', []]];
    for (const [name, v] of VALUES) sets.push([`(${name} x${n})`, new Array(n).fill(v)]);
    return sets;
}

let calls = 0;
function call(path, fn, self, construct) {
    const made = [];
    for (const [label, args] of argSets(fn)) {
        writeFile(lastFile, `${path} ${label}`);
        if (trace) console.log(`FUZZ_CALL ${path} ${label}`);
        calls++;
        try {
            const r = construct ? new fn(...args) : fn.apply(self, args);
            if (r && typeof r.then === 'function') r.then(() => {}, () => {});
            // A handle it made (a constructor's, a factory's): fuzzed in turn.
            if (r && typeof r === 'object' && made.length < 2 && typeof r.then !== 'function') {
                const proto = Object.getPrototypeOf(r);
                if (proto && proto !== Object.prototype && proto !== Array.prototype) made.push(r);
            }
        } catch (e) {
            // the right answer
        }
    }
    return made;
}

function methodNames(obj) {
    const names = new Set();
    for (let p = Object.getPrototypeOf(obj); p && p !== Object.prototype; p = Object.getPrototypeOf(p)) {
        for (const k of Object.getOwnPropertyNames(p)) if (k !== 'constructor') names.add(k);
    }
    return [...names];
}

function isClass(fn) {
    if (!fn.prototype) return false;
    return Object.getOwnPropertyNames(fn.prototype).some((k) => k !== 'constructor') ||
        /^class\b/.test(Function.prototype.toString.call(fn));
}

function fuzzInstance(path, inst) {
    const names = methodNames(inst);
    const run = (suffix) => {
        for (const k of names) {
            if (SKIP.test(k)) continue;
            let m;
            try { m = inst[k]; } catch (e) { continue; }
            if (typeof m === 'function') call(`${path}${suffix}.${k}`, m, inst, false);
        }
    };
    run('');
    // Lifetime: every method again on an instance its owner released.
    for (const k of ['dispose', 'destroy', 'free', 'release', 'delete']) {
        if (typeof inst[k] === 'function') {
            writeFile(lastFile, `${path}.${k}()`);
            try { inst[k](); } catch (e) { /* fine */ }
            run(`<${k}d>`);
            break;
        }
    }
}

const seen = new Set();
function walk(path, obj, depth) {
    if (obj === null || (typeof obj !== 'object' && typeof obj !== 'function') || seen.has(obj)) return;
    seen.add(obj);
    let keys;
    try { keys = Object.getOwnPropertyNames(obj); } catch (e) { return; }
    for (const k of keys) {
        if (SKIP.test(k) || k === 'prototype' || k === 'constructor' || k === 'caller' || k === 'arguments') continue;
        let v;
        try { v = obj[k]; } catch (e) { continue; }
        const p = `${path}.${k}`;
        if (typeof v === 'function') {
            if (isClass(v)) {
                for (const inst of call(p, v, undefined, true)) fuzzInstance(`${p}#instance`, inst);
                walk(p, v, depth + 1);  // statics
            } else {
                for (const inst of call(p, v, obj, false)) fuzzInstance(`${p}()`, inst);
            }
        } else if (v && typeof v === 'object' && depth < 3) {
            walk(p, v, depth + 1);
        }
    }
}

walk(`bro.${ns}`, bro[ns], 0);
flush();
writeFile(lastFile, `done ${calls}`);
console.log(`FUZZ_DONE bro.${ns} ${calls} calls`);
