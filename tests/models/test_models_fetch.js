// bro.models.fetch / bro.models.available (docs/models-api.js) against a local
// HTTP server standing in for Hugging Face: the same tree and resolve paths,
// served by brokit's net module on loopback. No real network.
//
// Covered: a source without the folder falls through to the next; a folder
// with a subdirectory, an LFS file (sha256) and plain files (git blob sha1);
// progress; a connection that keeps dropping fails the fetch and leaves no
// usable folder, and the next fetch resumes from the bytes on disk (a Range
// request); a sha mismatch fails and leaves nothing usable; an abort mid-file;
// an installed model answers at once; bad names; available().
//
// Installs go to the per-user models dir, which BRO_APP_HOME makes a scratch
// one (tests/run_tests.sh sets it for tests/models).

const fs = require('fs');
const net = require('net');
const path = require('path');

if (!process.env.BRO_APP_HOME) {
    skipTest('needs a scratch BRO_APP_HOME (tests/run_tests.sh sets one): a fetch installs into the per-user models dir');
}

const userDir = bro.models.dirs[bro.models.dirs.length - 1];
assert(path.resolve(userDir) === path.resolve(process.env.BRO_APP_HOME, 'models'),
    `BRO_APP_HOME puts the per-user models dir under it: ${userDir}`);
assert(typeof bro.models.fetch === 'function' && typeof bro.models.available === 'function', 'fetch and available exist');
assert(JSON.stringify(bro.models.sources) === JSON.stringify(['wlejon/brovisionml-data', 'wlejon/brosoundml-data']),
    `the default sources: ${JSON.stringify(bro.models.sources)}`);

// ---- content -----------------------------------------------------------------
function bytes(n, seed) {
    const b = new Uint8Array(n);
    let x = seed >>> 0;
    for (let i = 0; i < n; i++) { x = (Math.imul(x, 1664525) + 1013904223) >>> 0; b[i] = x >>> 24; }
    return b;
}
const enc = (s) => new TextEncoder().encode(s);
const hex = (buf) => Array.from(new Uint8Array(buf), (v) => v.toString(16).padStart(2, '0')).join('');
async function sha256(b) { return hex(await crypto.subtle.digest('SHA-256', b)); }
async function blobSha1(b) {
    const head = enc(`blob ${b.length}\0`);
    const all = new Uint8Array(head.length + b.length);
    all.set(head, 0); all.set(b, head.length);
    return hex(await crypto.subtle.digest('SHA-1', all));
}

// path in the dataset -> { data, lfs }
const files = new Map();
const add = (p, data, lfs) => files.set(p, { data, lfs });
add('m1/weights.bin', bytes(3 * 1024 * 1024 + 123, 1), true);
add('m1/config.json', enc('{"model_type":"test"}\n'), false);
add('m1/sub/extra.txt', enc('nested file\n'), false);
add('bad/weights.bin', bytes(200000, 2), true);
add('slow/weights.bin', bytes(2 * 1024 * 1024, 3), true);

// The listing a tree request answers, as HF shapes it.
async function entry(p) {
    const f = files.get(p);
    const e = { type: 'file', path: p, size: f.data.length, oid: await blobSha1(f.data) };
    if (f.lfs) e.lfs = { oid: await sha256(f.data), size: f.data.length, pointerSize: 134 };
    return e;
}
async function listing(dir) {
    const out = [];
    const subdirs = new Set();
    for (const p of files.keys()) {
        if (!p.startsWith(dir + '/')) continue;
        const rest = p.slice(dir.length + 1);
        if (rest.includes('/')) subdirs.add(dir + '/' + rest.split('/')[0]);
        else out.push(await entry(p));
    }
    for (const d of subdirs) out.push({ type: 'directory', oid: '0', size: 0, path: d });
    return out;
}
const listings = new Map();
for (const d of ['m1', 'm1/sub', 'bad', 'slow']) listings.set(d, await listing(d));
// The listing for bad/ promises other bytes than the server sends.
listings.get('bad')[0].lfs.oid = '0'.repeat(64);

// ---- the server ------------------------------------------------------------
// mode: 'ok' | 'drop' (half of what is asked, then the connection dies) | 'stall'
const server = { mode: 'ok', ranges: [], gets: [], stalled: [] };
const tcp = net.createServer((sock) => {
    let buf = '';
    sock.on('error', () => {});
    sock.on('data', (chunk) => {
        buf += new TextDecoder().decode(chunk);
        const end = buf.indexOf('\r\n\r\n');
        if (end < 0) return;
        const head = buf.slice(0, end).split('\r\n');
        buf = '';
        const target = decodeURIComponent(head[0].split(' ')[1]);
        const rangeLine = head.find((l) => /^range:/i.test(l));
        respond(sock, target, rangeLine ? rangeLine.split(':')[1].trim() : null);
    });
});
tcp.listen(0);
const port = tcp.address().port;
const base = `http://127.0.0.1:${port}`;

function send(sock, status, headers, body) {
    const h = [`HTTP/1.1 ${status}`, 'Connection: close', ...Object.entries(headers).map(([k, v]) => `${k}: ${v}`)];
    sock.write(h.join('\r\n') + '\r\n\r\n');
    if (body) sock.write(body);
}

function respond(sock, target, range) {
    let m = /^\/api\/datasets\/t\/(\w+)\/tree\/main\/(.+)$/.exec(target);
    if (m) {
        const l = m[1] === 'repo' ? listings.get(m[2]) : null;
        if (!l) { send(sock, '404 Not Found', { 'Content-Length': 22 }, '{"error":"not found"}\n'); sock.end(); return; }
        const body = enc(JSON.stringify(l));
        send(sock, '200 OK', { 'Content-Type': 'application/json', 'Content-Length': body.length }, body);
        sock.end();
        return;
    }
    m = /^\/datasets\/t\/repo\/resolve\/main\/(.+)$/.exec(target);
    const f = m ? files.get(m[1]) : null;
    if (!f) { send(sock, '404 Not Found', { 'Content-Length': 0 }); sock.end(); return; }
    server.gets.push(m[1]);
    let from = 0;
    if (range) {
        server.ranges.push(`${m[1]} ${range}`);
        from = Number(/bytes=(\d+)-/.exec(range)[1]);
    }
    let data = f.data.subarray(from);
    if (m[1].startsWith('bad/')) data = bytes(data.length, 99);  // not what the listing hashed
    const headers = { 'Content-Type': 'application/octet-stream', 'Content-Length': data.length };
    const status = range ? '206 Partial Content' : '200 OK';
    if (range) headers['Content-Range'] = `bytes ${from}-${f.data.length - 1}/${f.data.length}`;
    if (server.mode === 'drop') {
        send(sock, status, headers, data.subarray(0, Math.max(1, data.length >> 1)));
        setTimeout(() => sock.destroy(), 30);
        return;
    }
    if (server.mode === 'stall') {
        send(sock, status, headers, data.subarray(0, 65536));
        server.stalled.push(sock);  // never finished
        return;
    }
    send(sock, status, headers, data);
    sock.end();
}

// A first source that has nothing, then the one that has the folders.
bro.settings.setDefault('models.sources', `${base}/datasets/t/empty, ${base}/datasets/t/repo`);
assert(JSON.stringify(bro.models.sources) === JSON.stringify([`${base}/datasets/t/empty`, `${base}/datasets/t/repo`]),
    `the setting's sources in order: ${JSON.stringify(bro.models.sources)}`);

const rejection = async (p) => { try { await p; return null; } catch (e) { return e; } };
const staging = (name) => path.join(userDir, '.bro-fetch', name);
const partFiles = (dir) => {
    const out = [];
    const walk = (d) => {
        if (!fs.existsSync(d)) return;
        for (const n of fs.readdirSync(d)) {
            const p = path.join(d, n);
            if (fs.statSync(p).isDirectory()) walk(p);
            else out.push(path.relative(dir, p).split(path.sep).join('/'));
        }
    };
    walk(dir);
    return out.sort();
};

// ---- available ---------------------------------------------------------------
const av = await bro.models.available('m1');
assert(av && av.source === `${base}/datasets/t/repo` && av.files === 3 &&
    av.bytes === 3 * 1024 * 1024 + 123 + 22 + 12, `available(m1) from the second source: ${JSON.stringify(av)}`);
assert((await bro.models.available('nothing-here')) === null, 'available of a missing folder is null');

// ---- bad names -----------------------------------------------------------------
for (const bad of ['', '../m1', 'a/../../m1', 42]) {
    const e = await rejection(bro.models.fetch(bad));
    assert(e instanceof TypeError, `fetch(${JSON.stringify(bad)}) rejects with a TypeError (${e})`);
}

// ---- not found -------------------------------------------------------------------
let e = await rejection(bro.models.fetch('nothing-here'));
assert(e && e.code === 'not-found' && /none of the model sources/.test(e.message), `a missing folder: ${e && e.message}`);

// ---- a connection that keeps dropping: fails, nothing usable, then resumes ----------
server.mode = 'drop';
e = await rejection(bro.models.fetch('m1'));
assert(e && e.code === 'network', `a dropping connection fails the fetch as network: ${e && e.message}`);
assert(bro.models.find('m1') === null, 'a failed fetch leaves no folder find() sees');
assert(!fs.existsSync(path.join(userDir, 'm1')), 'no m1 folder in the models dir');
const left = partFiles(staging('m1'));
assert(left.some((f) => f.endsWith('.part')), `the partial bytes stay staged for a resume: ${JSON.stringify(left)}`);
assert(server.ranges.length > 0, `retries resumed with Range requests: ${JSON.stringify(server.ranges)}`);

server.mode = 'ok';
server.ranges = [];
const progress = [];
const dir = await bro.models.fetch('m1', { onProgress: (p) => progress.push(p) });
assert(dir && path.resolve(dir) === path.resolve(userDir, 'm1'), `fetch resolves to the installed folder: ${dir}`);
assert(bro.models.find('m1') === dir, 'find() now answers the installed folder');
assert(server.ranges.some((r) => r.startsWith('m1/weights.bin bytes=')), `the second fetch resumed: ${JSON.stringify(server.ranges)}`);
for (const [p, f] of files) {
    if (!p.startsWith('m1/')) continue;
    const got = fs.readFileSync(path.join(dir, ...p.slice(3).split('/')));
    assert(got.length === f.data.length && (await sha256(got)) === (await sha256(f.data)), `${p} installed intact`);
}
assert(JSON.stringify(partFiles(dir)) === JSON.stringify(['config.json', 'sub/extra.txt', 'weights.bin']),
    `exactly the folder's files, no .part left: ${JSON.stringify(partFiles(dir))}`);
assert(!fs.existsSync(staging('m1')), 'the staging folder is gone');
assert(progress.length >= 1, 'onProgress was called');
const last = progress[progress.length - 1];
assert(last.done === last.total && last.total === av.bytes, `the last progress is complete: ${JSON.stringify(last)}`);
for (let i = 1; i < progress.length; i++) {
    assert(progress[i].done >= progress[i - 1].done && progress[i].total === last.total, 'progress only grows');
}

// Installed: answered at once, nothing fetched.
const getsBefore = server.gets.length;
assert((await bro.models.fetch('m1')) === dir && server.gets.length === getsBefore, 'an installed model needs no fetch');

// ---- a sha mismatch: fails, nothing usable, the bad bytes not kept ---------------------
e = await rejection(bro.models.fetch('bad'));
assert(e && e.code === 'integrity' && /does not match/.test(e.message), `a sha mismatch fails as integrity: ${e && e.message}`);
assert(bro.models.find('bad') === null && !fs.existsSync(path.join(userDir, 'bad')), 'no bad folder');
assert(!partFiles(staging('bad')).length, `the mismatched bytes are deleted: ${JSON.stringify(partFiles(staging('bad')))}`);

// ---- an abort mid-file --------------------------------------------------------------
server.mode = 'stall';
const ac = new AbortController();
const aborted = rejection(bro.models.fetch('slow', {
    signal: ac.signal,
    onProgress: (p) => { if (p.done > 0) ac.abort(); },
}));
e = await aborted;
assert(e && e.name === 'AbortError', `an abort rejects with an AbortError: ${e && (e.name + ' ' + e.message)}`);
assert(bro.models.find('slow') === null, 'an aborted fetch leaves no folder');
for (const s of server.stalled) s.destroy();
const pre = new AbortController();
pre.abort();
e = await rejection(bro.models.fetch('slow', { signal: pre.signal }));
assert(e && e.name === 'AbortError', 'an already-aborted signal rejects at once');

// The aborted one resumes too.
server.mode = 'ok';
server.ranges = [];
const slowDir = await bro.models.fetch('slow');
assert(slowDir && bro.models.find('slow') === slowDir, 'the aborted model installs on the next fetch');
assert(server.ranges.some((r) => r.startsWith('slow/weights.bin')), `and resumed: ${JSON.stringify(server.ranges)}`);

tcp.close();
console.log('test_models_fetch PASSED');
