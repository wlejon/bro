// Map / Set / Date / RegExp / Error / DataView survive worker.postMessage.
//
// Regression: none of these had a branch in the structured-clone serializer,
// so every one fell through to the plain-object writer and arrived as {}. No
// error, no warning — a Map sent to a worker became an empty object, and the
// only symptom was downstream code reading undefined off it.
//
// The genuinely non-cloneable objects (Promise, WeakMap/WeakSet/WeakRef) hit
// the same silent-{} path and now throw at send time instead, which is what
// real structured clone does.

const workerPath = '../workers/worker_postmessage_types.js';

const w = new Worker(workerPath);
let got = null;
w.onmessage = (e) => { got = e.data; };

const when = new Date('2020-03-04T05:06:07.008Z');
const map = new Map([['a', 1], ['b', 2], ['c', 3]]);
const set = new Set(['x', 'y', 'z']);
const re = /ab+c/gi;
re.lastIndex = 5;                       // spec: not carried across a clone
const err = new TypeError('kaboom');
const dv = new DataView(new ArrayBuffer(16), 4, 8);
dv.setInt32(0, 987654321);

w.postMessage({ when, map, set, re, err, dv });

const deadline = Date.now() + 15000;
while (got === null && Date.now() < deadline) { advanceTime(16); wallSleep(2); }
assert(got !== null, 'worker replied within budget');

// --- Date -------------------------------------------------------------------
assert(got.seen.when.kind === 'Date',
       'Date arrived as a Date, got ' + got.seen.when.kind);
assert(got.seen.when.time === when.getTime(),
       'Date kept its time value, got ' + got.seen.when.time);
assert(got.echoed.when instanceof Date, 'Date is still a Date coming back');
assert(got.echoed.when.getTime() === when.getTime(), 'Date survived the return leg');

// --- Map --------------------------------------------------------------------
assert(got.seen.map.kind === 'Map', 'Map arrived as a Map, got ' + got.seen.map.kind);
assert(got.seen.map.size === 3, 'Map kept 3 entries, got ' + got.seen.map.size);
assert(JSON.stringify(got.seen.map.entries) === JSON.stringify([['a','1'],['b','2'],['c','3']]),
       'Map kept its entries in insertion order, got ' + JSON.stringify(got.seen.map.entries));
assert(got.echoed.map instanceof Map, 'Map is still a Map coming back');
assert(got.echoed.map.get('b') === 2, 'Map values survived the return leg');

// --- Set --------------------------------------------------------------------
assert(got.seen.set.kind === 'Set', 'Set arrived as a Set, got ' + got.seen.set.kind);
assert(got.seen.set.size === 3, 'Set kept 3 values, got ' + got.seen.set.size);
assert(got.seen.set.values.join(',') === 'x,y,z',
       'Set kept insertion order, got ' + got.seen.set.values.join(','));
assert(got.echoed.set instanceof Set, 'Set is still a Set coming back');
assert(got.echoed.set.has('y'), 'Set membership survived the return leg');

// --- RegExp -----------------------------------------------------------------
assert(got.seen.re.kind === 'RegExp', 'RegExp arrived as a RegExp, got ' + got.seen.re.kind);
assert(got.seen.re.source === 'ab+c', 'RegExp kept its source, got ' + got.seen.re.source);
assert(got.seen.re.flags.split('').sort().join('') === 'gi',
       'RegExp kept its flags, got ' + got.seen.re.flags);
assert(got.seen.re.lastIndex === 0,
       'RegExp lastIndex reset per spec, got ' + got.seen.re.lastIndex);
assert(got.echoed.re instanceof RegExp, 'RegExp is still a RegExp coming back');
assert(got.echoed.re.test('xxabbbc'), 'the cloned RegExp actually matches');

// --- Error ------------------------------------------------------------------
assert(got.seen.err.kind === 'Error', 'Error arrived as an Error, got ' + got.seen.err.kind);
assert(got.seen.err.name === 'TypeError', 'Error kept its name, got ' + got.seen.err.name);
assert(got.seen.err.message === 'kaboom', 'Error kept its message, got ' + got.seen.err.message);
assert(got.seen.err.isTypeError === true, 'a TypeError is still a TypeError on the far side');
assert(got.seen.err.stack.length > 0, 'Error carried a stack across');
assert(got.echoed.err instanceof Error, 'Error is still an Error coming back');

// --- DataView ---------------------------------------------------------------
assert(got.seen.dv.kind === 'DataView', 'DataView arrived as a DataView, got ' + got.seen.dv.kind);
assert(got.seen.dv.byteLength === 8, 'DataView kept its length, got ' + got.seen.dv.byteLength);
assert(got.seen.dv.byteOffset === 4, 'DataView kept its offset, got ' + got.seen.dv.byteOffset);
assert(got.seen.dv.first === 987654321,
       'DataView kept the bytes at its window, got ' + got.seen.dv.first);
assert(got.echoed.dv instanceof DataView, 'DataView is still a DataView coming back');

// --- nesting: these types inside arrays/objects and inside each other -------
let nested = null;
w.onmessage = (e) => { nested = e.data; };
w.postMessage({
    box: { list: [new Date(1000), new Set([1, 2])], inner: new Map([['k', new Date(2000)]]) },
});
const d2 = Date.now() + 15000;
while (nested === null && Date.now() < d2) { advanceTime(16); wallSleep(2); }
assert(nested !== null, 'worker replied to the nesting probe');
const box = nested.echoed.box;
assert(box.list[0] instanceof Date && box.list[0].getTime() === 1000,
       'a Date nested in an array survived');
assert(box.list[1] instanceof Set && box.list[1].has(2),
       'a Set nested in an array survived');
assert(box.inner instanceof Map, 'a Map nested in an object survived');
assert(box.inner.get('k') instanceof Date && box.inner.get('k').getTime() === 2000,
       'a Date nested inside a Map value survived');

// --- non-cloneable values throw instead of silently becoming {} -------------
function refuses(label, make) {
    let threw = null;
    try {
        w.postMessage({ v: make() });
    } catch (e) {
        threw = e;
    }
    assert(threw !== null, label + ' throws rather than cloning to {}');
}

refuses('Promise', () => Promise.resolve(1));
refuses('WeakMap', () => new WeakMap());
refuses('WeakSet', () => new WeakSet());
refuses('WeakRef', () => new WeakRef({}));
refuses('function', () => function () {});

// The channel still works after a refused send.
let after = null;
w.onmessage = (e) => { after = e.data; };
w.postMessage({ when: new Date(4242) });
const d3 = Date.now() + 15000;
while (after === null && Date.now() < d3) { advanceTime(16); wallSleep(2); }
assert(after !== null, 'the worker channel survived the refused sends');
assert(after.echoed.when.getTime() === 4242, 'and still round-trips correctly');

w.terminate();

console.log('PASS test_postmessage_types');
