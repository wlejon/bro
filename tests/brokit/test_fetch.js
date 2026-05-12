// Test fetch and supporting classes.

assert(typeof fetch === 'function', 'fetch is fn');
assert(typeof Headers === 'function', 'Headers is fn');
assert(typeof Request === 'function', 'Request is fn');
assert(typeof Response === 'function', 'Response is fn');

// Headers construction
const h = new Headers({ 'Content-Type': 'application/json', 'X-Foo': 'bar' });
assert(h.get('Content-Type') === 'application/json', 'headers get');
assert(h.get('content-type') === 'application/json', 'headers case-insensitive');
assert(h.has('X-Foo'), 'headers has');
h.set('X-Foo', 'baz');
assert(h.get('X-Foo') === 'baz', 'headers set');
h.delete('X-Foo');
assert(!h.has('X-Foo'), 'headers delete');

// Request
const req = new Request('https://example.com/', { method: 'POST', body: 'hi' });
assert(req.url === 'https://example.com/', 'request url: ' + req.url);
assert(req.method === 'POST', 'request method: ' + req.method);

// Response
const res = new Response('body text', { status: 201, statusText: 'Created' });
assert(res.status === 201, 'response status: ' + res.status);
assert(res.statusText === 'Created', 'response statusText: ' + res.statusText);
assert(res.ok === true, 'response ok 201');
let txt;
res.text().then(t => { txt = t; });
flush(); advanceTime(10); flush();
assert(txt === 'body text', 'response text: ' + txt);

// Response with !ok
const r2 = new Response('', { status: 404 });
assert(r2.ok === false, 'ok false for 404');

// Bad URL fetch — should reject, not crash
let rejected = false;
let rejectedReason = null;
fetch('http://localhost:1/nonexistent')
    .then(() => { rejected = false; })
    .catch((e) => { rejected = true; rejectedReason = e; });
flush();
// give it some time
for (let i = 0; i < 20; i++) { advanceTime(50); flush(); sleep(50); flush(); }
assert(rejected, 'fetch to bad url rejected (got: ' + (rejectedReason && rejectedReason.message) + ')');

// data: URL (documented to be supported)
let dataRes;
fetch('data:text/plain,hello').then(r => r.text()).then(t => { dataRes = t; });
flush(); advanceTime(10); flush();
assert(dataRes === 'hello', 'data url fetch: ' + dataRes);
