// The file probe: Blob, File, FileReader, URL, and the round trip that is the
// whole point of them.
//
// Everything else in this layer reads: the app names a path and the host loads
// it. This is the only surface where the app HOLDS the bytes — it generated
// them, or decoded them, or was handed them — and needs somewhere to put them
// that the rest of the platform will accept. The round trip at the end is
// therefore the load-bearing assertion, not the constructors above it:
//
//     bytes -> new Blob -> URL.createObjectURL -> fetch(url) -> the same bytes
//
// A `blob:` URL that mints but does not resolve is the failure this is here to
// catch, and it is a quiet one — every step reports success and the fetch
// answers 404 for a resource that was never a file. The probe checks the URL is
// gone after revoke for the same reason: a table that only ever grows is a leak
// nothing else would notice.
//
// WHY EVERY ASYNC RESULT IS RECORDED AND PRINTED AT THE END, rather than from
// the callback that produced it. There are four independent chains here — a
// fetch chain, three Blob promises, eight FileReaders — and the order their
// lines would interleave is a property of how many microtask hops each happens
// to take. Pinning that interleaving would make the expectation a recording of
// one implementation's scheduling rather than a statement of what must be true,
// and it would fail on any change that added or removed a hop without breaking
// anything.
//
// So the values are collected and printed in a fixed order from the final
// frame, and the SEQUENCING claims that actually matter are asserted directly
// instead:
//
//   - a read must not deliver synchronously (`sync.before`), because
//     `reader.onload` is assigned after `readAsText` is called, which is how
//     every FileReader ever written is structured;
//   - load must precede loadend on one reader (`read.order`);
//   - an aborted read must deliver NOTHING (`read.deliveries`), because the
//     program has already said it does not want the result.
//
// EVERY LINE IS `APP <name>=<value>`, every value an integer, a boolean or a
// string this file chose — the expectation beside it is written from what must
// be true, not recorded from a run (tests/bronze_host/README.md).

function say(label, value) { console.log('APP ' + label + '=' + value); }

// ---------------------------------------------------------------------------
// Blob: bytes, from every kind of part
// ---------------------------------------------------------------------------

const b1 = new Blob(['hello']);
say('blob.size', b1.size);
say('blob.typeEmpty', b1.type === '');

const b2 = new Blob(['hello ', 'world'], { type: 'text/plain' });
say('blob.joinedSize', b2.size);
say('blob.type', b2.type);

// A typed array, an ArrayBuffer, and another Blob are all legal parts.
const bytes = new Uint8Array([65, 66, 67]);          // "ABC"
const b3 = new Blob([bytes, b1, new Uint8Array([33]).buffer]);
say('blob.mixedSize', b3.size);   // 3 + 5 + 1

// slice, including the negative offsets the spec spells out.
const cut = b2.slice(6, 11);
say('blob.sliceSize', cut.size);
const tail = b2.slice(-5);
say('blob.negSliceSize', tail.size);
// A slice takes a fresh type, or none.
say('blob.sliceTypeEmpty', cut.type === '');
say('blob.sliceTyped', b2.slice(0, 5, 'text/x-thing').type);

// ---------------------------------------------------------------------------
// File: a Blob with a name
// ---------------------------------------------------------------------------

const f = new File(['model data'], 'scene.gltf',
                   { type: 'model/gltf+json', lastModified: 1700000000000 });
say('file.name', f.name);
say('file.size', f.size);
say('file.type', f.type);
say('file.lastModified', f.lastModified);
say('file.relPathEmpty', f.webkitRelativePath === '');
// A File IS a Blob everywhere it matters, which is what lets file-input code
// hand one to the same reader a generated Blob goes to.
say('file.slices', f.slice(0, 5).size);

// ---------------------------------------------------------------------------
// URL: minting, resolving, revoking
// ---------------------------------------------------------------------------

const url = URL.createObjectURL(b2);
say('url.isBlobScheme', url.indexOf('blob:') === 0);
// Two blobs must never share a URL — a counter that did not advance would make
// the second revoke free the first one's bytes.
say('url.distinct', URL.createObjectURL(b1) !== url);

// URL.parse rather than `new URL(...)`: a host function cannot carry a static,
// so URL here is a namespace and the constructor has no place to live. See
// src/bronze_host/host_file.cpp — the runtime message is quoted there.
const u = URL.parse('https://example.com:8080/a/b/../c.json?x=1&y=two#frag');
say('parse.protocol', u.protocol);
say('parse.hostname', u.hostname);
say('parse.port', u.port);
say('parse.host', u.host);
// The `..` is removed: a resolved URL that still carries one is not equal to
// what every other implementation produces.
say('parse.pathname', u.pathname);
say('parse.search', u.search);
say('parse.hash', u.hash);
say('parse.origin', u.origin);
say('parse.href', u.href);
say('parse.param', u.searchParams.get('y'));
say('parse.paramMissing', u.searchParams.get('nope') === null);
say('parse.has', u.searchParams.has('x'));

// A relative reference against a base, which is the case a loader hits.
const rel = URL.parse('textures/wood.png', 'https://example.com/models/scene/');
say('rel.href', rel.href);
const up = URL.parse('../shared/a.bin', 'https://example.com/models/scene/x.gltf');
say('rel.upHref', up.href);
const abs = URL.parse('/root.txt', 'https://example.com/deep/path/');
say('rel.absHref', abs.href);

// ---------------------------------------------------------------------------
// Everything below is asynchronous, so it RECORDS rather than prints
// ---------------------------------------------------------------------------

const got = {};                 // name -> value, printed in a fixed order at the end
let deliveries = 0;             // every terminal FileReader callback bumps this
let syncDone = false;           // flipped after the last synchronous statement
let deliveredEarly = false;     // any callback that ran before that flip

function record(name, value) {
    if (!syncDone) deliveredEarly = true;
    got[name] = value;
}

// ---------------------------------------------------------------------------
// The round trip: does a minted URL actually RESOLVE?
// ---------------------------------------------------------------------------
// This is the assertion the rest of the file exists to set up. fetch() and the
// <img> loader read the ENGINE's object-URL table (util/object_url.h), the same
// one the page's own scripts write to — a private table here would have minted
// URLs that only compiled code could read, and an <img> would have shown
// nothing with no error anywhere.

fetch(url).then(function (resp) {
    record('trip.ok', resp.ok);
    record('trip.status', resp.status);
    record('trip.contentType', resp.headers.get('content-type'));
    return resp.text();
}).then(function (text) {
    record('trip.text', text);
    // And back the other way: a fetched body becomes a Blob again.
    return fetch(url).then(function (r) { return r.blob(); });
}).then(function (blob) {
    record('trip.blobSize', blob.size);
    record('trip.blobType', blob.type);
    // Revoked means gone. A table that only grows is a leak nothing else in
    // the process would ever report.
    URL.revokeObjectURL(url);
    return fetch(url);
}).then(function (resp) {
    record('trip.afterRevoke', resp.status);
}).catch(function (e) {
    record('trip.threw', String(e));
});

// A data: URL carries its own bytes and must take the same path — no file, no
// 404. It used to be refused by name in the image loader.
fetch('data:text/plain;base64,aGkh').then(function (r) {
    record('data.status', r.status);
    return r.text();
}).then(function (t) {
    record('data.text', t);
});

// ---------------------------------------------------------------------------
// Blob's own promise readers
// ---------------------------------------------------------------------------

b2.text().then(function (t) { record('promise.text', t); });
b2.arrayBuffer().then(function (ab) { record('promise.abLen', ab.byteLength); });
b2.bytes().then(function (u8) {
    record('promise.u8Len', u8.length);
    record('promise.u8First', u8[0]);
});

// ---------------------------------------------------------------------------
// FileReader
// ---------------------------------------------------------------------------

say('reader.emptyState', new FileReader().readyState);

const r1 = new FileReader();
r1.onload = function () { deliveries++; record('read.text', r1.result); };
r1.readAsText(b2);
// LOADING, immediately: the read was started and has not delivered.
say('reader.loadingState', r1.readyState);

const r2 = new FileReader();
r2.addEventListener('load', function () {
    deliveries++;
    record('read.bufLen', r2.result.byteLength);
    record('read.doneState', r2.readyState);
});
r2.readAsArrayBuffer(b3);

const r3 = new FileReader();
r3.onload = function () { deliveries++; record('read.dataURL', r3.result); };
r3.readAsDataURL(new Blob(['hi!'], { type: 'text/plain' }));

const r4 = new FileReader();
r4.onload = function () { deliveries++; record('read.binary', r4.result); };
r4.readAsBinaryString(new Blob([new Uint8Array([104, 105])]));

// loadend fires for a successful read too, and after load.
const r5 = new FileReader();
let order5 = '';
r5.onload = function () { deliveries++; order5 = order5 + 'load,'; };
r5.onloadend = function () { record('read.order', order5 + 'loadend'); };
r5.readAsText(b1);

// An aborted read delivers NOTHING: the queued task finds a generation that is
// not its own and publishes no result. Without that latch the reader would
// deliver a result the program has already said it does not want — which is why
// this onload bumps the counter the expectation pins rather than printing.
const r6 = new FileReader();
r6.onload = function () { deliveries++; record('read.abortedDelivered', 'MUST NOT'); };
r6.readAsText(b1);
r6.abort();
say('reader.abortedState', r6.readyState);
say('reader.abortedResultNull', r6.result === null);

// A second read started before the first delivered wins, for the same reason:
// one terminal event per read, and the first read is no longer the current one.
const r7 = new FileReader();
r7.onload = function () { deliveries++; record('read.lastWins', r7.result); };
r7.readAsText(new Blob(['first']));
r7.readAsText(new Blob(['second']));

// Reading something that is not a Blob is an error event, not a throw.
const r8 = new FileReader();
r8.onerror = function () { deliveries++; record('read.notABlob', r8.error.name); };
r8.readAsText('just a string');

syncDone = true;

// ---------------------------------------------------------------------------
// Done, several frames later
// ---------------------------------------------------------------------------
// Later than the other probes: the reads and the fetch chain settle across
// microtask checkpoints and host-task drains. Four frames is well past what any
// of them needs; a value still missing at that point prints as `absent`, which
// is a visible failure rather than a hang.

const REPORT = [
    'trip.ok', 'trip.status', 'trip.contentType', 'trip.text',
    'trip.blobSize', 'trip.blobType', 'trip.afterRevoke', 'trip.threw',
    'data.status', 'data.text',
    'promise.text', 'promise.abLen', 'promise.u8Len', 'promise.u8First',
    'read.text', 'read.bufLen', 'read.doneState', 'read.dataURL',
    'read.binary', 'read.order', 'read.lastWins', 'read.notABlob',
    'read.abortedDelivered',
];

let frames = 0;
function tick() {
    if (++frames < 4) { requestAnimationFrame(tick); return; }
    // The sequencing claims first, then the values.
    say('sync.noEarlyDelivery', deliveredEarly === false);
    say('read.deliveries', deliveries);
    for (const name of REPORT) {
        say(name, Object.prototype.hasOwnProperty.call(got, name) ? got[name] : 'absent');
    }
    // Printed last, so a probe that died halfway is a missing line rather than
    // a silently short but otherwise matching output.
    say('done', 1);
}
requestAnimationFrame(tick);
