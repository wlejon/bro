// Test TextEncoder/TextDecoder and atob/btoa.

const enc = new TextEncoder();
const dec = new TextDecoder();

// ASCII
const a = enc.encode('hello');
assert(a instanceof Uint8Array, 'encode returns Uint8Array');
assert(a.length === 5, 'ASCII length: ' + a.length);
assert(a[0] === 104, 'h = 104: ' + a[0]);
assert(dec.decode(a) === 'hello', 'decode ASCII');

// UTF-8 multibyte
const u = enc.encode('héllo');
assert(u.length === 6, 'héllo utf-8 is 6 bytes: ' + u.length);
assert(dec.decode(u) === 'héllo', 'utf-8 round trip');

// Emoji (surrogate pair → 4 bytes)
const e = enc.encode('🌍');
assert(e.length === 4, 'emoji 4 bytes: ' + e.length);
assert(dec.decode(e) === '🌍', 'emoji round trip');

// Empty
assert(enc.encode('').length === 0, 'empty encode');
assert(dec.decode(new Uint8Array(0)) === '', 'empty decode');

// btoa / atob
const b64 = btoa('hello');
assert(b64 === 'aGVsbG8=', 'btoa hello: ' + b64);
assert(atob(b64) === 'hello', 'atob round trip');

// Padding
assert(btoa('a') === 'YQ==', 'btoa a: ' + btoa('a'));
assert(btoa('ab') === 'YWI=', 'btoa ab: ' + btoa('ab'));
assert(btoa('abc') === 'YWJj', 'btoa abc: ' + btoa('abc'));
assert(atob('YQ==') === 'a', 'atob YQ==');
assert(atob('YWI=') === 'ab', 'atob YWI=');
