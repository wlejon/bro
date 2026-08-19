// The interpreter bridge: `new Function(source)` in COMPILED code, compiled by
// the engine's QuickJS realm, and the value boundary that makes the result
// usable (src/bronze_host/host_interp.h).
//
// Every line is `APP <name>=<value>` and every expectation beside it was
// derived from the spec and from what the bridge promises BEFORE the first
// run — so a passing check is evidence the bridge matches the model rather
// than a recording of what the build happened to print.
//
// What only this check catches: that bronze's refusal of dynamic code is
// delegated rather than merely suppressed, and that a value handed through the
// resulting function keeps its identity, its liveness and its type on the way
// back.

function say(label, value) {
    console.log('APP ' + label + '=' + value);
}

// --- The refusal is gone, and what replaces it computes ---------------------

const add = new Function('a', 'b', 'return a + b');
say('add', add(2, 3));
say('isFunction', typeof add);
// 27.3.1.1 names these `anonymous`, and library code reads the name.
say('name', add.name);

// A body with no parameters, and one whose parameter list is a single string
// holding both names — the spec joins every argument but the last with commas,
// so the two spellings are the same function.
say('noParams', new Function('return 41 + 1')());
say('joinedParams', new Function('a, b', 'return a * b')(6, 7));

// --- Compiled values, seen from the interpreted side ------------------------

const target = { n: 1, label: 'start', nested: { deep: 5 } };

// Read a property of a compiled object.
const readIt = new Function('o', 'return o.n + o.nested.deep');
say('read', readIt(target));

// Write one, and see it in compiled code afterwards: the wrapper forwards, it
// does not copy, so this is the SAME object and not a snapshot of it.
const writeIt = new Function('o', 'o.n = 99; o.added = "yes";');
writeIt(target);
say('writeSeen', target.n);
say('writeAdded', target.added);

// Call a compiled method, with `this` arriving as the object it was read from.
const obj = {
    factor: 10,
    scale: function (x) { return x * this.factor; },
};
const callIt = new Function('o', 'return o.scale(4)');
say('methodCall', callIt(obj));

// Identity survives a round trip: what comes back is the object that went in,
// not a wrapper around a wrapper.
const identity = new Function('o', 'return o');
say('roundTrip', identity(target) === target);
say('roundTripTwice', identity(identity(target)) === target);

// Enumeration reaches across: Object.keys of a compiled object answers the
// compiled object's own keys.
const keysOf = new Function('o', 'return Object.keys(o).join(",")');
say('keys', keysOf({ a: 1, b: 2, c: 3 }));

// `in` and property deletion.
const hasIt = new Function('o', 'return ("n" in o) + "," + ("missing" in o)');
say('has', hasIt(target));

// --- Interpreted values, seen from the compiled side ------------------------

// The shape the three.js editor's player actually uses: a function that builds
// and returns an object of functions, which the compiled program then calls.
const makeApi = new Function(
    'base',
    'return { init: function (n) { return base + n; }, name: "api" };'
);
const api = makeApi(100);
say('apiName', api.name);
say('apiInit', api.init(5));

// ...and the same call reached through `bind`, which is the exact spelling in
// editor/js/libs/app.js — the returned function must be a REAL function object
// for the language's own bind to apply to it.
const boundMaker = new Function('a', 'return this.tag + a').bind({ tag: 'T' });
say('bound', boundMaker('!'));

// An interpreted array comes back as something compiled code can read.
const makeArray = new Function('return [1, 2, 3]');
const arr = makeArray();
say('arrayLen', arr.length);
say('arrayIndex', arr[1]);

// --- Binary data ------------------------------------------------------------
//
// Copied, not shared: embed.h's pointer contract makes a shared buffer a
// dangling read at the next allocation, so a mutation on one side is NOT
// visible on the other. That is a documented property of the bridge, so it is
// asserted rather than worked around.

const sum = new Function('a', 'var t = 0; for (var i = 0; i < a.length; i++) t += a[i]; return t;');
const f32 = new Float32Array([1.5, 2.5, 3.0]);
say('typedIn', sum(f32));

const makeTyped = new Function('return new Float32Array([4, 5, 6])');
const out = makeTyped();
say('typedOutLen', out.length);
say('typedOutValue', out[2]);

const mutate = new Function('a', 'a[0] = 1000;');
mutate(f32);
say('typedIsCopy', f32[0]);

// --- Errors, both directions ------------------------------------------------

let caught = 'none';
try {
    new Function('throw new Error("from the interpreter")')();
} catch (e) {
    caught = 'caught';
}
say('throwOut', caught);

// A compiled function that throws, called from interpreted code, must surface
// as a throw there too rather than as a silent undefined.
const thrower = { boom: function () { throw new Error('from compiled code'); } };
const callThrower = new Function('o', 'try { o.boom(); return "no"; } catch (e) { return "yes"; }');
say('throwIn', callThrower(thrower));

// A syntax error in the source is the constructor's own throw, not a crash.
let syntax = 'none';
try {
    new Function('this is not javascript');
} catch (e) {
    syntax = 'threw';
}
say('syntaxError', syntax);

// --- The other three constructors -------------------------------------------
//
// %AsyncFunction% and the generator forms are distinct intrinsics, and the
// hook is told which one it is answering. A host that gave all four the same
// answer would hand back a plain function where the program is about to
// `await` or `next()` it.

const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
const asyncFn = new AsyncFunction('return 7');
say('asyncIsPromise', typeof asyncFn().then);

const GeneratorFunction = Object.getPrototypeOf(function* () {}).constructor;
const genFn = new GeneratorFunction('yield 1; yield 2;');
const it = genFn();
say('generatorFirst', it.next().value);
say('generatorSecond', it.next().value);

// --- A library holding a compiled callback -----------------------------------
//
// This is the three.js editor's actual shape, and the only one where BOTH
// directions are live inside a single call: interpreted signals.js keeps the
// compiled listener, and dispatching hands it a compiled object. The listener
// has to be a function to the interpreted `typeof` guard, `.apply` has to
// reach it, `arguments` has to survive the trip out, and the object has to
// arrive as itself rather than as undefined.

const makeBus = new Function(
    'return {' +
    '  subs: [],' +
    '  on: function (f) { this.subs.push(f); return typeof f; },' +
    '  emit: function () {' +
    '    var a = Array.prototype.slice.call(arguments);' +
    '    for (var i = 0; i < this.subs.length; i++) this.subs[i].apply(null, a);' +
    '    return a.length;' +
    '  }' +
    '};'
);
const bus = makeBus();

const payload = { domElement: 'canvas' };
let seen = 'never';
let seenCount = -1;
say('busTypeof', bus.on(function (r) {
    seenCount = arguments.length;
    seen = (r === payload) ? r.domElement : ('wrong:' + r);
}));
say('busEmitCount', bus.emit(payload));
say('busArg', seen);
say('busArity', seenCount);

// Two arguments, the second a primitive, because a dispatch that dropped
// everything past the first would still pass the test above.
let pair = 'never';
bus.subs.length = 0;
bus.on(function (o, n) { pair = (o === payload) + ':' + n; });
bus.emit(payload, 7);
say('busPair', pair);

// `call` and direct invocation reach a compiled function too, not just `apply`.
const invoke = new Function('f', 'o', 'return f.call(null, o) + "|" + f(o);');
say('busInvoke', invoke(function (o) { return o.domElement; }, payload));

say('done', 'true');
