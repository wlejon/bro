// Test crypto.randomUUID and crypto.getRandomValues.

assert(typeof crypto === 'object', 'crypto exists');
assert(typeof crypto.randomUUID === 'function', 'randomUUID is fn');

const uuid = crypto.randomUUID();
assert(typeof uuid === 'string', 'uuid is string: ' + typeof uuid);
const re = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
assert(re.test(uuid), 'uuid format v4: ' + uuid);

// Two UUIDs should differ
const u1 = crypto.randomUUID();
const u2 = crypto.randomUUID();
assert(u1 !== u2, 'two uuids differ');

// getRandomValues
assert(typeof crypto.getRandomValues === 'function', 'getRandomValues is fn');
const buf = new Uint8Array(256);
const out = crypto.getRandomValues(buf);
assert(out === buf, 'getRandomValues returns same array');

// Statistical: at least 200 nonzero bytes in 256 (chance of failure ≈ 0)
let nonzero = 0;
for (let i = 0; i < buf.length; i++) if (buf[i] !== 0) nonzero++;
assert(nonzero > 200, 'entropy: ' + nonzero + '/256 nonzero');

// Two fills should differ
const b1 = new Uint8Array(64);
const b2 = new Uint8Array(64);
crypto.getRandomValues(b1);
crypto.getRandomValues(b2);
let same = 0;
for (let i = 0; i < 64; i++) if (b1[i] === b2[i]) same++;
assert(same < 32, 'two fills differ: ' + same + '/64 same');

// Different typed arrays
const u32 = new Uint32Array(8);
crypto.getRandomValues(u32);
let any = false;
for (let i = 0; i < u32.length; i++) if (u32[i] !== 0) { any = true; break; }
assert(any, 'Uint32Array filled');
