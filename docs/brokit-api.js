// =============================================================================
// brokit API Reference
// =============================================================================
//
// brokit provides the standard runtime APIs available to all bro JS contexts.
// APIs fall into two categories:
//
//   Node-compatible modules — available via require():
//     const fs = require('fs');           // or require('node:fs')
//     const path = require('path');       // or require('node:path')
//     const os = require('os');           // or require('node:os')
//     const cp = require('child_process'); // or require('node:child_process')
//
//   Web-compatible globals — available on globalThis, matching browser APIs:
//     fetch, URL, URLSearchParams, crypto, WebSocket, EventSource,
//     TextEncoder, TextDecoder, ReadableStream, WritableStream,
//     Blob, FormData, AbortController, EventTarget, MessageChannel,
//     localStorage, sessionStorage, indexedDB, navigator, console,
//     setTimeout, setInterval, structuredClone, atob, btoa, etc.
//
// All APIs are installed by brokit::api::installAll(ctx) before any user JS
// runs. require() maps module names to their __brokit_* globals.
//
// =============================================================================


// =============================================================================
// NODE-COMPATIBLE MODULES (via require)
// =============================================================================


// -----------------------------------------------------------------------------
// fs — require('fs')
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


// -----------------------------------------------------------------------------
// path — require('path')
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
// os — require('os')
// -----------------------------------------------------------------------------

const os = require('os');

os.platform();           // → 'win32' | 'linux' | 'darwin'
os.arch();               // → 'x64' | 'arm64' | ...
os.homedir();            // → home directory path
os.tmpdir();             // → temp directory path
os.hostname();           // → machine hostname
os.EOL;                  // '\r\n' on Windows, '\n' on Linux


// -----------------------------------------------------------------------------
// child_process — require('child_process')
// -----------------------------------------------------------------------------

const cp = require('child_process');

cp.execSync(command, options?);                    // → stdout string. Throws on non-zero exit
cp.exec(command, options?, callback?);             // callback(err, stdout, stderr) or Promise<{ stdout, stderr }>
cp.execFileSync(file, args?, options?);            // like execSync but takes file + args array
cp.execFile(file, args?, options?, callback?);     // async version of execFileSync
cp.spawnSync(command, args?, options?);            // → { stdout, stderr, status, ... }


// =============================================================================
// WEB-COMPATIBLE GLOBALS
// =============================================================================


// -----------------------------------------------------------------------------
// fetch
// -----------------------------------------------------------------------------

fetch(url, options?);    // → Promise<Response>
// options: { method, headers, body, signal }
// Response: { ok, status, statusText, headers, text(), json(), arrayBuffer(), blob() }

// Supporting classes:
new Headers(init?);
new Request(url, options?);
new Response(body?, options?);


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

new Blob(parts?, options?);              // options: { type }
// Properties: size, type
// Methods: text(), arrayBuffer(), slice(start?, end?, type?), stream()


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

process.platform;        // 'win32' | 'linux' | 'darwin'
process.cwd();
process.exit(code?);
process.env;             // Proxy — reads/writes/deletes environment variables


// -----------------------------------------------------------------------------
// navigator (global)
// -----------------------------------------------------------------------------

navigator.userAgent;
navigator.language;
navigator.languages;
navigator.platform;
navigator.onLine;


// -----------------------------------------------------------------------------
// Other globals
// -----------------------------------------------------------------------------

structuredClone(value);
atob(base64);                            // base64 → binary string
btoa(string);                            // binary string → base64
queueMicrotask(callback);
