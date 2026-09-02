// =============================================================================
// brokit API Reference
// =============================================================================
//
// brokit provides the standard runtime APIs available to all bro JS contexts.
// APIs fall into two categories:
//
//   Node-compatible modules, available via require():
//     const fs = require('fs');           // or require('node:fs')
//     const path = require('path');       // or require('node:path')
//     const os = require('os');           // or require('node:os')
//     const cp = require('child_process'); // or require('node:child_process')
//     const net = require('net');         // raw TCP client + server
//     const dgram = require('dgram');     // raw UDP sockets
//
//   Your own files, require() also loads JS and JSON off disk, so an app's JS
//   can be split across files instead of living in one script:
//     const { photoFeats } = require('./photo_feats.js');  // .js optional
//     const config = require('./config.json');             // parsed
//     const shared = require('../lib/shared');             // ../ and absolute too
//
//   Relative paths resolve against the directory of the file that CALLS require
//   (as in Node), so a module can require its own siblings wherever it is loaded
//   from. A module is evaluated once and cached by resolved path: two requires of
//   the same file share one instance, so module-level state is real state. Both
//   `module.exports = {...}` and `exports.foo = ...` work, and the module sees
//   __filename / __dirname.
//
//   Web-compatible globals, available on globalThis, matching browser APIs:
//     fetch, URL, URLSearchParams, crypto, WebSocket, WebSocketServer, EventSource,
//     TextEncoder, TextDecoder, ReadableStream, WritableStream,
//     CompressionStream, DecompressionStream,
//     Blob, FormData, AbortController, EventTarget, MessageChannel,
//     TreeWalker, NodeFilter, localStorage, sessionStorage, indexedDB,
//     navigator, console, setTimeout, setInterval, structuredClone,
//     atob, btoa, process, FastNoise, etc.
//
// All APIs are installed by brokit::api::installAll(ctx) before any user JS
// runs. require() maps module names to their __brokit_* globals.
//
// =============================================================================


// =============================================================================
// NODE-COMPATIBLE MODULES (via require)
// =============================================================================


// -----------------------------------------------------------------------------
// fs, require('fs')
// -----------------------------------------------------------------------------

const fs = require('fs');

// ── Sync ──

fs.readFileSync(path, encoding);         // encoding: 'utf-8' | undefined (→ ArrayBuffer)
fs.writeFileSync(path, data, encoding);
fs.appendFileSync(path, data, encoding);
fs.statSync(path);                       // → { size, mtimeMs, mode, mtime, isFile(), isDirectory(), isSymbolicLink() }
fs.lstatSync(path);                      // like statSync but does not follow symlinks
fs.readdirSync(path, options);           // options: { withFileTypes: true } → Dirent[]
fs.existsSync(path);                     // → boolean
fs.mkdirSync(path, options);             // options: { recursive: true }
fs.rmdirSync(path);
fs.rmSync(path, options);               // options: { recursive: true, force: true }
fs.unlinkSync(path);
fs.renameSync(oldPath, newPath);
fs.copyFileSync(src, dest);
fs.chmodSync(path, mode);
fs.realpathSync(path);

// ── Async (callback or Promise) ──

fs.readFile(path, encoding, callback?);  // callback(err, data) or returns Promise
fs.writeFile(path, data, encoding, callback?);
fs.appendFile(path, data, encoding, callback?);
fs.stat(path, callback?);
fs.lstat(path, callback?);
fs.readdir(path, options, callback?);
fs.mkdir(path, options, callback?);
fs.rmdir(path, callback?);
fs.rm(path, options, callback?);
fs.unlink(path, callback?);
fs.rename(oldPath, newPath, callback?);
fs.copyFile(src, dest, callback?);
fs.chmod(path, mode, callback?);
fs.realpath(path, callback?);

// ── Promises ──

fs.promises.readFile(path, encoding);
fs.promises.writeFile(path, data, encoding);
fs.promises.appendFile(path, data, encoding);
fs.promises.stat(path);
fs.promises.lstat(path);
fs.promises.readdir(path, options);
fs.promises.mkdir(path, options);
fs.promises.rmdir(path);
fs.promises.rm(path, options);
fs.promises.unlink(path);
fs.promises.rename(oldPath, newPath);
fs.promises.copyFile(src, dest);
fs.promises.chmod(path, mode);
fs.promises.realpath(path);
fs.promises.access(path);               // resolves or rejects with ENOENT

// ── Constants ──

fs.constants.F_OK;  // 0
fs.constants.R_OK;  // 4
fs.constants.W_OK;  // 2
fs.constants.X_OK;  // 1
fs.constants.COPYFILE_EXCL;          // 1
fs.constants.COPYFILE_FICLONE;       // 2
fs.constants.COPYFILE_FICLONE_FORCE; // 4

// ── Watch (native FSWatcher) ──
//
// Backed by inotify (Linux), FSEvents (macOS) and ReadDirectoryChangesW
// (Windows). Each watcher owns one OS thread that pushes events into a
// lock-free ring; the engine's per-frame tick drains the ring and dispatches
// 'change' / 'error' callbacks on the JS thread.
//
//   const w = fs.watch(path, options?, listener?);
//
//   options: {
//     recursive: boolean,   // include subdirectories (default false)
//     persistent: boolean,  // accepted for Node compat, currently ignored
//   }
//   listener: (eventType, filename) => void
//     eventType: 'rename' (created/deleted/moved) | 'change' (modified)
//     filename:  basename when non-recursive; forward-slash path relative to
//                the watched root when recursive
//
//   w.on('change', (eventType, filename) => ...);
//   w.on('error', (err) => ...);     // err.code === 'EWATCHER'
//   w.on('close', () => ...);
//   w.off(event, listener);
//   w.close();                       // idempotent
//
// Events are coalesced by the OS, not by us, a single editor save may
// produce one or several 'change' events depending on how the editor writes
// (truncate-then-write, atomic-rename, etc).

const watcher = fs.watch('./assets', { recursive: true }, function (event, filename) {
    console.log(event, filename);
});
watcher.on('error', err => console.error('watch error:', err.message));


// -----------------------------------------------------------------------------
// path, require('path')
// -----------------------------------------------------------------------------

const path = require('path');

path.join(...segments);
path.resolve(...segments);
path.normalize(p);
path.isAbsolute(p);
path.dirname(p);
path.basename(p, ext?);
path.extname(p);
path.parse(p);           // → { root, dir, base, ext, name }
path.format(obj);        // inverse of parse
path.sep;                // '\\' on Windows, '/' on Linux
path.delimiter;          // ';' on Windows, ':' on Linux


// -----------------------------------------------------------------------------
// os, require('os')
// -----------------------------------------------------------------------------

const os = require('os');

os.platform();           // → 'win32' | 'linux' | 'darwin'
os.arch();               // → 'x64' | 'arm64' | ...
os.homedir();            // → home directory path
os.tmpdir();             // → temp directory path
os.hostname();           // → machine hostname
os.EOL;                  // '\r\n' on Windows, '\n' on Linux


// -----------------------------------------------------------------------------
// child_process, require('child_process')
// -----------------------------------------------------------------------------

const cp = require('child_process');

cp.execSync(command, options?);                    // → stdout string. Throws on non-zero exit. BLOCKS
cp.exec(command, options?, callback?);             // callback(err, stdout, stderr) or Promise<{ stdout, stderr }>
cp.execFileSync(file, args?, options?);            // like execSync but takes file + args array. BLOCKS
cp.execFile(file, args?, options?, callback?);     // async version of execFileSync
cp.spawnSync(command, args?, options?);            // → { stdout, stderr, status, ... }. BLOCKS
cp.spawn(file, args?, options?);                   // → ChildProcess (non-blocking, see below)

// ── Which of these involves a shell (it matters) ──
//
// COMMAND STRING → a shell parses it:   exec, execSync
//   `&`, `|`, `>`, and builtins all work, and quoting is yours to get right.
//   Never paste a user-supplied value into one of these strings.
//
// FILE + ARGV ARRAY → no shell at all:  execFile, execFileSync, spawn, spawnSync
//   Entries are passed to the child verbatim, so a path holding `&`, `(`, `)`,
//   `^` or a quote arrives intact and cannot be executed as syntax. This is the
//   form to use for anything the user chose — a media filename like
//   `mix & master (final).mov` is ordinary, and only this form survives it.
//   Pass `{ shell: true }` to spawn/spawnSync if you deliberately want the
//   shell back (builtins, redirects).
//
// Only the *Sync calls block. exec and execFile run on top of spawn, so the
// frame loop keeps running while the child works — safe to await from a UI.

// ── cp.spawn: long-running children ──
//
// options: { cwd, env, stdio, encoding, highWaterMark, stdoutFile, stderrFile }
//   stdio         'pipe' to stream output and write stdin. DEFAULT IS 'ignore'
//                 (no pipes at all) — this diverges from Node deliberately, so
//                 an existing caller that never reads can't silently buffer,
//                 and a GUI child keeps its own window. Opt in explicitly.
//   encoding      with stdio:'pipe', 'utf8' delivers decoded strings (UTF-8
//                 safe across chunk boundaries). Default is binary Uint8Array,
//                 which is what raw pixel/audio streams need.
//   highWaterMark per-stream buffer cap, default 8 MB. When full the reader
//                 stops and the child blocks in write() — real backpressure.
//                 Draining happens automatically on the poll tick. A single
//                 read never overshoots it, so a very small value also caps
//                 read size and costs throughput: for raw video, size it to a
//                 few frames (w*h*4 bytes each) rather than leaving the
//                 default. Measured headroom is multiple GB/s, well clear of
//                 the ~237 MB/s a 1080p30 RGBA feed needs.
//   env           REPLACES the child environment (Node semantics).
//   stdoutFile /  redirect output to files instead (truncate on open; same
//   stderrFile    path for both = combined log). Ignored when stdio:'pipe'.
//
// ChildProcess:
//   .pid .killed .exitCode .signal
//   .kill(signal?)                       // 'SIGTERM' default, 'SIGKILL' to force
//   .on('exit',  (code, signal) => {})   // process is gone
//   .on('close', (code, signal) => {})   // gone AND both pipes drained to EOF
//   with stdio:'pipe':
//   .stdout / .stderr   .on('data', chunk) / .on('end') / .setEncoding(enc)
//   .stdin              .write(stringOrBytes) → bytes written; .end() sends EOF
//
// Every byte the child wrote is delivered before 'close' fires. Polling adapts:
// ~4 ms while bytes flow, 25 ms idle piped, 100 ms unpiped.

// Live progress from a long-running tool:
const enc = cp.spawn('ffmpeg', ['-i', 'in.mov', '-progress', 'pipe:1', 'out.mp4'],
                     { stdio: 'pipe', encoding: 'utf8' });
let tail = '';
enc.stdout.on('data', (chunk) => {
    tail += chunk;
    const lines = tail.split('\n');
    tail = lines.pop();                    // keep the partial line for next chunk
    for (const line of lines) {
        const [k, v] = line.split('=');
        if (k === 'frame') updateProgressBar(Number(v));
    }
});
enc.on('close', (code) => { if (code !== 0) showError(); });
// enc.kill() to cancel.

// Binary out (raw frames), and stdin in:
const dec = cp.spawn('ffmpeg', ['-i', 'clip.mp4', '-f', 'rawvideo',
                                '-pix_fmt', 'rgba', 'pipe:1'], { stdio: 'pipe' });
dec.stdout.on('data', (bytes) => { /* bytes is a Uint8Array */ });
dec.stdin.end();                           // EOF — tools reading pipe:0 need this


// -----------------------------------------------------------------------------
// net, require('net'), raw TCP client + server
// -----------------------------------------------------------------------------
//
// SECURITY: server.listen() binds 127.0.0.1 (loopback) unless a host is given
// explicitly. A listener is a real port on the user's machine. Pass
// '0.0.0.0' (or an interface address) only when you mean to expose it.
//
// v1 notes:
//   - Backpressure-naive: write() buffers unboundedly in native code and
//     always returns true; there is no 'drain' event. Do not stream gigabytes.
//   - allowHalfOpen is always false (Node's default): a received FIN auto-ends
//     the write side once pending data flushes.
//   - DNS in connect() resolves synchronously (instant for IPs/localhost; a
//     remote hostname blocks the JS thread for the lookup).
//   - Sockets are pumped from the engine frame loop (headless: advanceTime).

const net = require('net');

// ── Server ──
const server = net.createServer((socket) => { /* per-connection */ });
server.listen(port, host?, cb?);   // port 0 → ephemeral; host default 127.0.0.1
server.address();                  // → { address, port, family } (real port after listen)
server.close(cb?);                 // stops accepting; existing sockets live on
server.on('listening' | 'connection' | 'close' | 'error', fn);

// ── Client / socket ──
const sock = net.connect(port, host?, cb?);  // also net.createConnection; host default 127.0.0.1
sock.write(data);                  // string (UTF-8) | ArrayBuffer | TypedArray → true
sock.end(data?, cb?);              // flush, then FIN (graceful close)
sock.destroy();                    // immediate teardown
sock.address();                    // → { address, port, family }
sock.remoteAddress; sock.remotePort; sock.localAddress; sock.localPort;
sock.bytesRead; sock.bytesWritten;
sock.readyState;                   // 'opening' | 'open' | 'closed'
sock.on('connect', fn);            // connected (also 'ready')
sock.on('data', (chunk) => {});    // chunk: Buffer (a Uint8Array subclass)
sock.on('end', fn);                // peer sent FIN
sock.on('close', (hadError) => {});// always last
sock.on('error', (err) => {});     // followed by close(true)
// No-op compat shims: setNoDelay, setKeepAlive, setTimeout, pause, resume,
// ref, unref.

net.isIP(s);                       // → 4 | 6 | 0
net.isIPv4(s); net.isIPv6(s);      // → boolean

// Example: loopback echo
const srv = net.createServer((s) => s.on('data', (c) => s.write(c)));
srv.listen(0);
const c = net.connect(srv.address().port, '127.0.0.1', () => c.write('hi'));
c.on('data', (chunk) => { c.end(); srv.close(); });


// -----------------------------------------------------------------------------
// dgram, require('dgram'), raw UDP sockets
// -----------------------------------------------------------------------------
//
// SECURITY: bind() defaults to 127.0.0.1 (::1 for udp6) unless a host is
// given explicitly, pass '0.0.0.0' to receive from the network.
//
// v1 notes:
//   - 'udp4' and 'udp6' both work (same code path, AF_INET / AF_INET6).
//   - Broadcast is opt-in via setBroadcast(true).
//   - Multicast (addMembership etc.) is DEFERRED, not implemented.
//   - send() is fire-and-forget: a full kernel buffer drops the datagram
//     (faithful UDP); the callback reports only local errors.

const dgram = require('dgram');

const udp = dgram.createSocket('udp4');       // or 'udp6', or { type: 'udp4' }
udp.bind(port?, host?, cb?);                  // port 0/omitted → ephemeral; emits 'listening'
udp.send(data, port, host?, cb?);             // data: string | ArrayBuffer | TypedArray;
                                              // host default 127.0.0.1 (::1 for udp6)
udp.close(cb?);
udp.address();                                // → { address, port, family }
udp.setBroadcast(flag);
udp.on('message', (msg, rinfo) => {});        // msg: Buffer; rinfo: { address, family, port, size }
udp.on('listening' | 'close' | 'error', fn);

// Example: loopback round trip
const rx = dgram.createSocket('udp4');
rx.on('message', (msg, rinfo) => rx.close());
rx.bind(0);
const tx = dgram.createSocket('udp4');
tx.send('hello', rx.address().port);          // → rx 'message' fires


// =============================================================================
// WEB-COMPATIBLE GLOBALS
// =============================================================================


// -----------------------------------------------------------------------------
// fetch
// -----------------------------------------------------------------------------

fetch(url, options?);    // → Promise<Response>
// options:
//   method, 'GET' | 'POST' | 'PUT' | 'PATCH' | 'DELETE' | ...   (default 'GET')
//   headers, plain object { 'Content-Type': 'application/json', ... } or Headers
//   body, string, ArrayBuffer, or TypedArray (Blob/FormData not yet supported)
// Note: brokit's fetch does NOT yet implement signal/AbortSignal, keepalive, redirect,
// credentials, mode, cache, referrer, or integrity. Pass only the keys above.
//
// Response: {
//   ok, status, statusText, url,
//   headers,                  // Headers object (get/has/forEach)
//   body,                     // ReadableStream, pull chunks for streaming
//   bodyUsed,                 // becomes true after text()/json()/arrayBuffer()/blob()
//   text()       → Promise<string>,
//   json()       → Promise<any>,
//   arrayBuffer()→ Promise<ArrayBuffer>,
//   blob()       → Promise<Blob>,
//   clone()      → Response,
// }
// file:// and data: URLs are also supported (no network).

// Example, POST JSON
const res = await fetch('https://api.example.com/users', {
  method: 'POST',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ name: 'jonny' }),
});
if (!res.ok) throw new Error(res.statusText);
const user = await res.json();

// Example, stream a large response
const r = await fetch('https://example.com/big.bin');
const reader = r.body.getReader();
let total = 0;
while (true) {
  const { value, done } = await reader.read();
  if (done) break;
  total += value.byteLength;
}

// Supporting classes:
new Headers(init?);            // init: object, array of [k,v] pairs, or another Headers
new Request(url, options?);    // same options shape as fetch()
new Response(body?, options?); // options: { status, statusText, headers }


// -----------------------------------------------------------------------------
// URL / URLSearchParams
// -----------------------------------------------------------------------------

new URL(url, base?);
// Properties: href, origin, protocol, host, hostname, port, pathname, search, hash, username, password
// Methods: toString(), toJSON()

new URLSearchParams(init?);
// Methods: get, set, has, delete, append, entries, forEach, keys, values, toString, sort


// -----------------------------------------------------------------------------
// crypto
// -----------------------------------------------------------------------------

crypto.getRandomValues(typedArray);      // fill with random bytes
crypto.randomUUID();                     // → 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'

// ── crypto.subtle ──

crypto.subtle.digest(algorithm, data);                     // → Promise<ArrayBuffer>
crypto.subtle.importKey(format, keyData, algorithm, extractable, usages);
crypto.subtle.generateKey(algorithm, extractable, usages);
crypto.subtle.sign(algorithm, key, data);
crypto.subtle.verify(algorithm, key, signature, data);
crypto.subtle.encrypt(algorithm, key, data);
crypto.subtle.decrypt(algorithm, key, data);
crypto.subtle.exportKey(format, key);


// -----------------------------------------------------------------------------
// Encoding
// -----------------------------------------------------------------------------

new TextEncoder();                       // encode(string) → Uint8Array
new TextDecoder(encoding?);              // decode(buffer) → string

new TextEncoderStream();                 // TransformStream: string chunks → Uint8Array chunks
new TextDecoderStream(encoding?);        // TransformStream: Uint8Array chunks → string chunks


// -----------------------------------------------------------------------------
// Streams
// -----------------------------------------------------------------------------

new ReadableStream({ start, pull, cancel });
// Methods: getReader(), pipeThrough(transform), pipeTo(writable), tee(), cancel()
// Reader: read() → { value, done }, cancel(), releaseLock()

new WritableStream({ start, write, close, abort });
// Methods: getWriter(), abort()
// Writer: write(chunk), close(), abort(), releaseLock()

// ── Compression Streams ──
// Incremental DEFLATE codec (miniz-backed) behind the web-standard classes.
// format: 'gzip' (RFC 1952) | 'deflate' (ZLIB-wrapped, RFC 1950) |
//         'deflate-raw' (raw DEFLATE, RFC 1951), anything else → TypeError.
// Chunks written must be BufferSource (ArrayBuffer or view); other types
// error the stream with TypeError. Output arrives as Uint8Array chunks as
// the codec produces them (bounded ≤64 KiB, nothing buffers whole inputs).
// Decompression errors (corrupt data, truncated stream, trailing garbage,
// gzip crc/size mismatch) error the stream with TypeError.

new CompressionStream(format);           // { readable, writable }
new DecompressionStream(format);         // { readable, writable }

// Example: gzip round-trip
const gzipped = source.pipeThrough(new CompressionStream('gzip'));
const restored = gzipped.pipeThrough(new DecompressionStream('gzip'));


// -----------------------------------------------------------------------------
// WebSocket
// -----------------------------------------------------------------------------

const ws = new WebSocket(url, protocols?);
ws.readyState;           // WebSocket.CONNECTING (0), OPEN (1), CLOSING (2), CLOSED (3)
ws.send(data);           // string, ArrayBuffer, or Uint8Array
ws.close(code?, reason?);
ws.onopen = fn;
ws.onclose = fn;         // event: { code, reason, wasClean }
ws.onmessage = fn;       // event: { data }
ws.onerror = fn;
ws.addEventListener(type, listener);
ws.removeEventListener(type, listener);
ws.binaryType;           // 'blob' (default) or 'arraybuffer'


// -----------------------------------------------------------------------------
// WebSocketServer, RFC 6455 server (also require('websocket-server'))
// -----------------------------------------------------------------------------
//
// Serves ws:// connections on a raw TCP listener (the `net` module). Delivered
// sockets mirror the WebSocket CLIENT surface above, so handler code is
// symmetric across both ends.
//
// SECURITY: binds 127.0.0.1 unless options.host is given explicitly, a
// listener is a real port on the user's machine.
//
// v1 notes:
//   - No TLS serving (no wss:// listener; the CLIENT does support wss://).
//   - No permessage-deflate (the extension offer is simply not acknowledged).
//   - Subprotocols are not negotiated (Sec-WebSocket-Protocol ignored).
//   - Client frames MUST be masked (RFC 6455): unmasked input fails the
//     connection with close code 1002. Server frames are never masked.
//   - Fragmented messages are reassembled; ping frames are answered with pong
//     automatically; close handshake carries code + reason both ways.

const wss = new WebSocketServer({ port, host? });  // port 0 → ephemeral
wss.address();                 // → { address, port, family }
wss.clients;                   // live server-side sockets (array)
wss.close(cb?);                // 1001 "going away" to clients, then stop listening
wss.on('listening', fn);
wss.on('connection', (ws, request) => {});
// request: { url, headers } from the HTTP upgrade (headers lowercased)
wss.on('close' | 'error', fn);

// The `ws` handed to 'connection' looks like a client WebSocket, already OPEN:
//   ws.readyState / ws.send(data) / ws.close(code?, reason?)
//   ws.onmessage / ws.onclose / ws.onerror (+ addEventListener)
//   ws.binaryType ('blob' default → Uint8Array messages; 'arraybuffer' → ArrayBuffer)
// plus server-side extras: ws.remoteAddress, ws.remotePort, ws.url, ws.headers.

// Example: echo server + the standard client, one process
const echo = new WebSocketServer({ port: 0 });
echo.on('connection', (ws) => { ws.onmessage = (ev) => ws.send(ev.data); });
const client = new WebSocket('ws://127.0.0.1:' + echo.address().port + '/');
client.onopen = () => client.send('hi');
client.onmessage = (ev) => { client.close(1000); echo.close(); };


// -----------------------------------------------------------------------------
// EventSource (Server-Sent Events)
// -----------------------------------------------------------------------------

const es = new EventSource(url, options?);
// options: { withCredentials }
es.readyState;           // EventSource.CONNECTING (0), OPEN (1), CLOSED (2)
es.close();
es.onopen = fn;
es.onmessage = fn;       // event: { data, lastEventId, type }
es.onerror = fn;
es.addEventListener(type, listener);
es.removeEventListener(type, listener);


// -----------------------------------------------------------------------------
// Blob
// -----------------------------------------------------------------------------

new Blob(parts?, options?);              // parts: array of (string | ArrayBuffer | TypedArray | Blob)
                                         // options: { type }, MIME type string
// Properties:
//   size                                  // total byte length
//   type                                  // MIME type (lowercase, '' if unset)
// Methods:
//   text()             → Promise<string>            // UTF-8 decode
//   arrayBuffer()      → Promise<ArrayBuffer>       // raw bytes copy
//   slice(start?, end?, contentType?) → Blob        // byte range; contentType overrides .type
// Note: brokit Blob does NOT implement .stream(). Use arrayBuffer() and wrap manually
// in a ReadableStream if you need a stream.

// Example
const b = new Blob(['hello, ', 'world'], { type: 'text/plain' });
b.size;                       // 12
b.type;                       // 'text/plain'
await b.text();               // 'hello, world'
const tail = b.slice(7);      // Blob of 'world'


// -----------------------------------------------------------------------------
// FormData
// -----------------------------------------------------------------------------

new FormData();
// Methods: append, set, get, getAll, has, delete, entries, forEach, keys, values


// -----------------------------------------------------------------------------
// AbortController / AbortSignal
// -----------------------------------------------------------------------------

const ac = new AbortController();
ac.signal;               // AbortSignal
ac.abort(reason?);
ac.signal.aborted;       // boolean
ac.signal.reason;
ac.signal.addEventListener('abort', fn);


// -----------------------------------------------------------------------------
// EventTarget
// -----------------------------------------------------------------------------

new EventTarget();
// Methods: addEventListener(type, listener), removeEventListener(type, listener), dispatchEvent(event)


// -----------------------------------------------------------------------------
// MessageChannel / MessagePort
// -----------------------------------------------------------------------------

const mc = new MessageChannel();
mc.port1;                // MessagePort
mc.port2;                // MessagePort
// MessagePort: postMessage(data), onmessage, close()


// -----------------------------------------------------------------------------
// Storage (localStorage / sessionStorage)
// -----------------------------------------------------------------------------

localStorage.getItem(key);
localStorage.setItem(key, value);
localStorage.removeItem(key);
localStorage.clear();
localStorage.key(index);
localStorage.length;

// localStorage persists to `<appDir>/.storage.json`: one JSON object of string
// values, rewritten whole (atomically, via a .tmp rename) on every setItem. It is
// a key/value store for small state — a save file belongs in its own file under
// bro.userDataDir, written with fs (see paths-api.js).
// sessionStorage has the same API (always in-memory, not persisted)


// -----------------------------------------------------------------------------
// IndexedDB
// -----------------------------------------------------------------------------

indexedDB.open(name, version?);          // → IDBOpenDBRequest
indexedDB.deleteDatabase(name);          // → IDBOpenDBRequest
// Full IDBDatabase / IDBTransaction / IDBObjectStore / IDBCursor API


// -----------------------------------------------------------------------------
// console
// -----------------------------------------------------------------------------

console.log(...args);
console.warn(...args);
console.error(...args);
console.info(...args);
console.debug(...args);
console.time(label);
console.timeEnd(label);
console.timeLog(label, ...args);


// -----------------------------------------------------------------------------
// Timers
// -----------------------------------------------------------------------------

setTimeout(callback, delay?, ...args);   // → id
clearTimeout(id);
setInterval(callback, delay?, ...args);  // → id
clearInterval(id);


// -----------------------------------------------------------------------------
// process (global)
// -----------------------------------------------------------------------------
//
// Minimal Node-compatible process global. Available everywhere, no require().

process.platform;        // 'win32' | 'linux' | 'darwin' (or 'unknown')
process.cwd();           // → string, current working directory
process.exit(code?);     // terminates the process; default code 0
process.env;             // Proxy, string-keyed environment variables
                         //   read:    process.env.PATH       → string | undefined
                         //   write:   process.env.FOO = 'x'  // updates real env (setenv)
                         //   delete:  delete process.env.FOO
                         //   has:     'FOO' in process.env

// Example
if (process.platform === 'win32') {
  console.log('home:', process.env.USERPROFILE);
} else {
  console.log('home:', process.env.HOME);
}
if (!process.env.API_KEY) process.exit(1);


// -----------------------------------------------------------------------------
// navigator (global)
// -----------------------------------------------------------------------------

navigator.userAgent;
navigator.language;
navigator.languages;
navigator.platform;
navigator.onLine;


// -----------------------------------------------------------------------------
// TreeWalker / NodeFilter
// -----------------------------------------------------------------------------
//
// DOM TreeWalker, implemented in JS over standard Node properties
// (childNodes, parentNode, nodeType). Works on bro's DOM and on any tree
// that exposes those members. Also installed as document.createTreeWalker
// when a document object is present.

new TreeWalker(root, whatToShow?, filter?);
// whatToShow: bitmask of NodeFilter.SHOW_* (default SHOW_ALL)
// filter:     function(node) → NodeFilter.FILTER_*  (or { acceptNode: fn })
//
// Properties:
//   root, whatToShow, filter, currentNode
// Navigation methods (each returns the new currentNode, or null):
//   parentNode(), firstChild(), lastChild(),
//   nextSibling(), previousSibling(),
//   nextNode(), previousNode()

NodeFilter.SHOW_ALL;          // 0xFFFFFFFF
NodeFilter.SHOW_ELEMENT;      // 0x1
NodeFilter.SHOW_TEXT;         // 0x4
NodeFilter.SHOW_COMMENT;      // 0x80
// (also SHOW_ATTRIBUTE, SHOW_CDATA_SECTION, SHOW_PROCESSING_INSTRUCTION,
//  SHOW_DOCUMENT, SHOW_DOCUMENT_TYPE, SHOW_DOCUMENT_FRAGMENT, ...)
NodeFilter.FILTER_ACCEPT;     // 1, yield this node
NodeFilter.FILTER_REJECT;     // 2, skip node and its subtree
NodeFilter.FILTER_SKIP;       // 3, skip node, but recurse into children

// Example, collect every <a> under document.body
const walker = new TreeWalker(
  document.body,
  NodeFilter.SHOW_ELEMENT,
  (n) => n.tagName === 'A' ? NodeFilter.FILTER_ACCEPT : NodeFilter.FILTER_SKIP,
);
const links = [];
let node;
while ((node = walker.nextNode())) links.push(node);


// -----------------------------------------------------------------------------
// Other globals
// -----------------------------------------------------------------------------

structuredClone(value);
atob(base64);                            // base64 → binary string
btoa(string);                            // binary string → base64
queueMicrotask(callback);


// -----------------------------------------------------------------------------
// FastNoise (global), FastNoise2 SIMD noise
// -----------------------------------------------------------------------------
//
// Metadata-driven binding to FastNoise2. Build a node graph (generators +
// modifiers + blends), then sample it, single points, dense 2D/3D grids,
// or seamless tileable 2D. Every node type registered with FastNoise2 is
// available via FastNoise.create(name); a few common ones also have named
// shortcuts on the FastNoise constructor.

// ── Construction ──

new FastNoise(encodedNodeTree);          // from a FastNoise2 node-tree string
                                         // (the format the FastNoise2 NoiseTool exports)
FastNoise.create(typeName);              // → FastNoise instance
                                         // typeName is any node from FastNoise.types()

// Convenience factories (equivalent to FastNoise.create('<Name>')):
FastNoise.Simplex();
FastNoise.SuperSimplex();
FastNoise.Perlin();
FastNoise.Value();
FastNoise.CellularValue();
FastNoise.CellularDistance();
FastNoise.CellularLookup();
FastNoise.FractalFBm();
FastNoise.FractalRidged();
FastNoise.DomainWarpGradient();

// Discovery
FastNoise.types();                       // → [{ name, groups }, ...] every registered type
                                         // (Simplex, Perlin, FractalFBm, FractalRidged,
                                         //  DomainWarp*, Add/Subtract/Multiply/Divide,
                                         //  Min/Max/MinSmooth/MaxSmooth, Fade, Remap,
                                         //  Terrace, Constant, Checkerboard, SineWave,
                                         //  Cellular*, GeneratorCache, SeedOffset, ...)

// ── Sampling ──

noise.genSingle2D(x, y, seed);                                  // → number
noise.genSingle3D(x, y, z, seed);                               // → number
noise.genUniformGrid2D(xOffset, yOffset, xSize, ySize,
                       frequency, seed);                        // → Float32Array(xSize*ySize)
noise.genUniformGrid3D(xOff, yOff, zOff, xSize, ySize, zSize,
                       frequency, seed);                        // → Float32Array(xSize*ySize*zSize)
noise.genUniformGrid3DInto(dest,                                // pre-allocated Float32Array
                           xOff, yOff, zOff,
                           xSize, ySize, zSize,
                           frequency, seed);                    // → undefined (writes in place)
noise.genTileable2D(xSize, ySize, frequency, seed);             // → Float32Array, seamless wrap

// ── Configuration (metadata-driven) ──
//
// FastNoise2 nodes have three kinds of members:
//   - Variables: named scalars (float, int, or enum). Enums accept the int
//                  index or the enum name string ("Manhattan", "Euclidean", ...)
//   - Nodes: typed source connections (e.g. FractalFBm's "Source")
//   - Hybrids: accept either a float OR a node (e.g. FractalFBm's "Gain")

noise.set(name, value);                   // assigns by member name
noise.getMembers();                       // → { type, variables, nodes, hybrids }
                                          //   variables[i]: { name, type, enumValues? }

// Per-dimension members ("Multiplier X", "Multiplier Y", ...) are addressed
// by appending the dimension letter: noise.set('Multiplier Y', 2.0).

// Example, programmatic terrain heightmap (broworkshop's demos/terrain uses this pattern)
const fbm = FastNoise.create('FractalFBm');
fbm.set('Source', FastNoise.create('Simplex'));
fbm.set('Octaves', 5);
fbm.set('Lacunarity', 2.0);
fbm.set('Gain', 0.5);
const heightmap = fbm.genUniformGrid2D(0, 0, 512, 512, 0.005, 1337);
// heightmap is a Float32Array(512*512), row-major

// Example, blend two fractals with a Fade node
const base = FastNoise.create('FractalFBm');
base.set('Source', FastNoise.create('Simplex'));
base.set('Octaves', 4);
const ridges = FastNoise.create('FractalRidged');
ridges.set('Source', FastNoise.create('Simplex'));
ridges.set('Octaves', 3);
const blend = FastNoise.create('Fade');
blend.set('A', base);
blend.set('B', ridges);
blend.set('Fade', 0.4);
const map = blend.genUniformGrid2D(0, 0, 256, 256, 0.01, 1337);

// Example, voxel density field, reusing one buffer per chunk
const dest = new Float32Array(32 * 32 * 32);
const vox = FastNoise.create('FractalRidged');
vox.set('Source', FastNoise.create('Perlin'));
vox.set('Octaves', 4);
vox.genUniformGrid3DInto(dest, chunkX*32, chunkY*32, chunkZ*32,
                         32, 32, 32, 0.05, 42);

// Example, load a graph designed in the FastNoise2 NoiseTool
const ENCODED = 'EwAAAIA/AAAAAAAAAEAAAACAQAAAACEABA==';
const designed = new FastNoise(ENCODED);
const v = designed.genSingle2D(10, 20, 1337);
