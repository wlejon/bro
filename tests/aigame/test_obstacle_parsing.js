// Obstacle fields ({x, z, hw, hd}) are read through a batched numeric
// property reader that walks the object's own properties directly. That fast
// path has to be indistinguishable from reading each name with a plain
// property get, so every shape it declines to handle is checked here against
// the plain-object result.

const G = bro.ai.game;

// The blocking wall every case below reproduces some other way.
const plain = [{ x: 5, z: 0, hw: 1, hd: 5 }];
assert(G.hasLineOfSight(0, 0, 10, 0, plain) === false, 'plain object blocks');
assert(G.hasLineOfSight(0, 20, 10, 20, plain) === true, 'plain object misses');

function sameAsPlain(obstacle, label) {
    assert(G.hasLineOfSight(0, 0, 10, 0, [obstacle]) === false, label + ': blocks');
    assert(G.hasLineOfSight(0, 20, 10, 20, [obstacle]) === true, label + ': misses');
}

// Accessor properties — not own data properties, so the fast path must defer.
// Counting the calls proves it deferred rather than merely landing on the
// right answer: a reader that mistook the accessor for a stored value would
// never invoke the getter at all.
let gets = 0;
sameAsPlain({
    get x() { gets++; return 5; },
    get z() { gets++; return 0; },
    get hw() { gets++; return 1; },
    get hd() { gets++; return 5; },
}, 'getters');
assert(gets >= 8, 'getters were actually invoked (saw ' + gets + ')');

// Mixed: some own data, some accessor.
sameAsPlain({ x: 5, z: 0, get hw() { return 1; }, hd: 5 }, 'mixed getter');

// Inherited fields live on the prototype, not on the object.
const proto = { x: 5, z: 0, hw: 1, hd: 5 };
sameAsPlain(Object.create(proto), 'inherited');
sameAsPlain(Object.assign(Object.create(proto), { x: 5 }), 'partly inherited');

// Values needing coercion: strings, booleans, boxed numbers, valueOf.
sameAsPlain({ x: '5', z: '0', hw: '1', hd: '5' }, 'string values');
sameAsPlain({ x: 5, z: false, hw: true, hd: 5 }, 'boolean values');
sameAsPlain({ x: new Number(5), z: 0, hw: 1, hd: 5 }, 'boxed number');
sameAsPlain({ x: { valueOf() { return 5; } }, z: 0, hw: 1, hd: 5 }, 'valueOf');

// Proxies are exotic — every read has to go through the trap.
let trapped = 0;
sameAsPlain(new Proxy(
    { x: 5, z: 0, hw: 1, hd: 5 },
    { get(t, k) { trapped++; return t[k]; } }
), 'proxy');
assert(trapped > 0, 'proxy get trap actually ran');

// Missing fields must read as 0 — not NaN, not junk, and not whatever the
// previous element left in the buffer. Asserted as equivalence to spelling
// the zeros out, so this tracks the parse and not the geometry.
function sameAs(a, b, label) {
    assert(G.hasLineOfSight(0, 0, 10, 0, [a]) === G.hasLineOfSight(0, 0, 10, 0, [b]),
           label + ': near ray');
    assert(G.hasLineOfSight(0, 20, 10, 20, [a]) === G.hasLineOfSight(0, 20, 10, 20, [b]),
           label + ': away from ray');
}
sameAs({ x: 5, z: 0 }, { x: 5, z: 0, hw: 0, hd: 0 }, 'absent hw/hd');
sameAs({}, { x: 0, z: 0, hw: 0, hd: 0 }, 'empty object');
sameAs({ hw: 1, hd: 5 }, { x: 0, z: 0, hw: 1, hd: 5 }, 'absent x/z');

// A populated element must not leak into a following sparse one.
assert(G.hasLineOfSight(0, 0, 10, 0, [{ x: 5, z: 0, hw: 1, hd: 5 }, {}]) ===
       G.hasLineOfSight(0, 0, 10, 0, [{ x: 5, z: 0, hw: 1, hd: 5 }]),
       'sparse element after a full one does not inherit its fields');

// A whole array of them, mixing shapes, must behave elementwise.
assert(G.hasLineOfSight(0, 0, 10, 0, [
    {},
    { x: 100, z: 100, hw: 1, hd: 1 },
    { get x() { return 5; }, z: 0, hw: 1, hd: 5 },
]) === false, 'mixed-shape array still finds the blocker');

// An object with far more properties than we ask for: the reader must pick
// out its four names rather than depending on shape order or position.
sameAsPlain({
    a: 1, b: 2, c: 3, hd: 5, d: 4, x: 5, e: 6, z: 0, f: 7, hw: 1, g: 8,
}, 'wide object');

console.log('PASS obstacle parsing');
