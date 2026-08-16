// Response.body is a ReadableStream, and a Response can be built from one.
//
// brokit reported `body === null` for every response that had a body, and threw
// away a stream passed to `new Response(stream)`. Both halves are what a loader
// with a progress bar does: read `response.body` chunk by chunk, wrap the
// chunks in a stream of its own, hand that to a new Response, then ask it for
// text or an ArrayBuffer. three.js's FileLoader — the base of GLTFLoader,
// FBXLoader, OBJLoader and the rest — does exactly this, and died on
// `response.body.getReader` before it read a single byte.

assert(typeof ReadableStream === 'function', 'ReadableStream exists');

// --- a fetched local file exposes its body as a stream ---------------------
let res = null;
fetch('index.html').then(function (r) { res = r; });
flush(); advanceTime(10); flush();
for (let i = 0; i < 20 && !res; i++) { advanceTime(50); flush(); sleep(10); flush(); }

assert(res !== null, 'the fetch settled');
assert(res.body !== null && res.body !== undefined,
       'a response with a body does not report body === null');
assert(typeof res.body.getReader === 'function',
       'and what it reports is a ReadableStream');

// --- read it the way a progress-reporting loader does ----------------------
let readBytes = null;
(async function () {
    const reader = res.body.getReader();
    const chunks = [];
    let total = 0;
    for (;;) {
        const r = await reader.read();
        if (r.done) break;
        chunks.push(r.value);
        total += r.value.byteLength;
    }
    const joined = new Uint8Array(total);
    let off = 0;
    for (const c of chunks) { joined.set(c, off); off += c.byteLength; }
    readBytes = joined;
})();
flush(); advanceTime(10); flush();
for (let i = 0; i < 20 && readBytes === null; i++) { advanceTime(50); flush(); sleep(10); flush(); }

assert(readBytes !== null, 'the body stream drained');
assert(readBytes.length > 0, 'and produced bytes, got ' + readBytes.length);
const asText = new TextDecoder().decode(readBytes);
assert(asText.indexOf('<') !== -1, 'the bytes are the document, got: ' +
       JSON.stringify(asText.slice(0, 40)));

// --- new Response(stream) is readable both ways -----------------------------
function streamOf(bytes) {
    let done = false;
    return new ReadableStream({
        pull: function (controller) {
            if (done) { controller.close(); return; }
            done = true;
            controller.enqueue(bytes);
        }
    });
}

let fromStreamText = null, fromStreamBytes = null;
new Response(streamOf(readBytes)).text().then(function (t) { fromStreamText = t; });
new Response(streamOf(readBytes)).arrayBuffer().then(function (b) { fromStreamBytes = b; });
flush(); advanceTime(10); flush();
for (let i = 0; i < 20 && (fromStreamText === null || fromStreamBytes === null); i++) {
    advanceTime(50); flush(); sleep(10); flush();
}

assert(fromStreamText === asText, 'text() drains a stream body');
assert(fromStreamBytes instanceof ArrayBuffer, 'arrayBuffer() returns an ArrayBuffer');
assert(fromStreamBytes.byteLength === readBytes.length,
       'byte-for-byte, got ' + fromStreamBytes.byteLength + ' of ' + readBytes.length);

// A constructed Response with plain bytes still gets a stream body.
const plain = new Response('hello');
assert(plain.body && typeof plain.body.getReader === 'function',
       'a string-bodied Response also exposes a stream');
const empty = new Response(null);
assert(empty.body === null, 'a bodyless Response keeps body === null');

console.log('PASS: Response bodies stream both ways');
