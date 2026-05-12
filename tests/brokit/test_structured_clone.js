// Test structuredClone.

assert(typeof structuredClone === 'function', 'structuredClone exists');

// Primitives
assert(structuredClone(42) === 42, 'number');
assert(structuredClone('hi') === 'hi', 'string');
assert(structuredClone(null) === null, 'null');
assert(structuredClone(undefined) === undefined, 'undefined');
assert(structuredClone(true) === true, 'bool');

// Array
const arr = [1, 2, 3];
const arrC = structuredClone(arr);
assert(arrC !== arr, 'array new ref');
assert(arrC.length === 3 && arrC[1] === 2, 'array values');

// Object (deep)
const obj = { a: 1, b: { c: 2 } };
const objC = structuredClone(obj);
assert(objC !== obj, 'obj new ref');
assert(objC.b !== obj.b, 'nested obj new ref');
assert(objC.b.c === 2, 'nested value');

// Mutate original — clone unaffected
obj.b.c = 999;
assert(objC.b.c === 2, 'deep clone independent: ' + objC.b.c);

// Date
const d = new Date(1700000000000);
const dC = structuredClone(d);
assert(dC instanceof Date, 'Date instance after clone');
assert(dC.getTime() === d.getTime(), 'Date time');
assert(dC !== d, 'Date new ref');

// Map
const m = new Map([['a', 1], ['b', 2]]);
const mC = structuredClone(m);
assert(mC instanceof Map, 'Map instance');
assert(mC.get('a') === 1 && mC.get('b') === 2, 'Map values');
assert(mC !== m, 'Map new ref');

// Set
const s = new Set([1, 2, 3]);
const sC = structuredClone(s);
assert(sC instanceof Set, 'Set instance');
assert(sC.has(2), 'Set has 2');
assert(sC !== s, 'Set new ref');

// Typed array
const u = new Uint8Array([1, 2, 3]);
const uC = structuredClone(u);
assert(uC instanceof Uint8Array, 'Uint8Array instance');
assert(uC.length === 3 && uC[1] === 2, 'Uint8Array values');
assert(uC.buffer !== u.buffer, 'Uint8Array new buffer');
