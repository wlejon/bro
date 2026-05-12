// Test URL and URLSearchParams.

const u = new URL('https://user:pass@example.com:8080/a/b?x=1&y=2#frag');
assert(u.protocol === 'https:', 'protocol: ' + u.protocol);
assert(u.hostname === 'example.com', 'hostname: ' + u.hostname);
assert(u.port === '8080', 'port: ' + u.port);
assert(u.host === 'example.com:8080', 'host: ' + u.host);
assert(u.pathname === '/a/b', 'pathname: ' + u.pathname);
assert(u.search === '?x=1&y=2', 'search: ' + u.search);
assert(u.hash === '#frag', 'hash: ' + u.hash);
assert(u.origin === 'https://example.com:8080', 'origin: ' + u.origin);
assert(u.username === 'user', 'username: ' + u.username);
assert(u.password === 'pass', 'password: ' + u.password);

// Relative URL with base
const u2 = new URL('/path', 'https://example.com');
assert(u2.href === 'https://example.com/path', 'relative resolve: ' + u2.href);

// toString
assert(typeof u.toString() === 'string', 'URL toString');

// URLSearchParams
const sp = new URLSearchParams('a=1&b=2&a=3');
assert(sp.get('a') === '1', 'get first a: ' + sp.get('a'));
assert(sp.getAll('a').length === 2, 'getAll a length');
assert(sp.has('b'), 'has b');
sp.set('a', 'X');
assert(sp.get('a') === 'X', 'after set a');
assert(sp.getAll('a').length === 1, 'set collapses duplicates');
sp.append('c', '4');
assert(sp.get('c') === '4', 'append c');
sp.delete('b');
assert(!sp.has('b'), 'delete b');
assert(typeof sp.toString() === 'string', 'sp toString');

// Encoding
const sp2 = new URLSearchParams();
sp2.set('q', 'hello world & friends');
const s = sp2.toString();
assert(s.indexOf(' ') === -1, 'spaces should be encoded: ' + s);
assert(s.indexOf('&friends') === -1 || s.indexOf('%26') !== -1, 'ampersand encoded: ' + s);

// Round-trip via new URLSearchParams
const sp3 = new URLSearchParams(s);
assert(sp3.get('q') === 'hello world & friends', 'decode round-trip: ' + sp3.get('q'));
