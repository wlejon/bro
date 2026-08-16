// FileReader, and the bridge that turns a dropped path into a real File.
//
// Blob and File come from brokit; what the web builds on top of them is here.
// FileReader is the older, event-driven half of the File API and the half that
// shipped code actually uses: three.js's editor reads every imported model
// through `new FileReader()` + readAsArrayBuffer, and so does most drag-and-drop
// import code written before Blob grew promises.
//
// Written in JS rather than C++ because it is exactly a wrapper: every read is
// `blob.arrayBuffer()` plus the event/state bookkeeping the interface specifies.

(function () {
    'use strict';

    if (typeof globalThis.Blob !== 'function') return;   // no brokit Blob here

    // ---------------------------------------------------------------- FileReader
    if (typeof globalThis.FileReader !== 'function') {
        const EMPTY = 0, LOADING = 1, DONE = 2;

        class FileReader extends EventTarget {
            constructor() {
                super();
                this.readyState = EMPTY;
                this.result = null;
                this.error = null;
                this.onloadstart = null;
                this.onprogress = null;
                this.onload = null;
                this.onloadend = null;
                this.onerror = null;
                this.onabort = null;
                this._aborted = false;
            }

            // The spec's event set, each also delivered to its on<name> handler.
            _fire(type, extra) {
                // Every FileReader event is a ProgressEvent: a progress bar
                // reads loaded/total off it, and the load handler reads
                // event.target.result (dispatchEvent sets target).
                const init = {
                    lengthComputable: !!(extra && extra.total > 0),
                    loaded: extra ? extra.loaded : 0,
                    total: extra ? extra.total : 0,
                };
                const ev = typeof ProgressEvent === 'function'
                    ? new ProgressEvent(type, init) : new Event(type);
                ev.lengthComputable = init.lengthComputable;
                ev.loaded = init.loaded;
                ev.total = init.total;
                const handler = this['on' + type];
                if (typeof handler === 'function') handler.call(this, ev);
                this.dispatchEvent(ev);
            }

            _read(blob, convert) {
                if (this.readyState === LOADING)
                    throw new Error('InvalidStateError: FileReader is already reading');
                this.readyState = LOADING;
                this.result = null;
                this.error = null;
                this._aborted = false;

                const size = (blob && typeof blob.size === 'number') ? blob.size : 0;
                this._fire('loadstart', { loaded: 0, total: size });

                const finish = (fn) => {
                    // A read always completes in a later turn, as it does on the
                    // web — code that assigns onload *after* calling read() (the
                    // common shape) still has its handler installed in time.
                    Promise.resolve().then(fn).catch((err) => {
                        if (this._aborted) return;
                        this.readyState = DONE;
                        this.result = null;
                        this.error = err instanceof Error ? err : new Error(String(err));
                        this._fire('error', { loaded: 0, total: size });
                        this._fire('loadend', { loaded: 0, total: size });
                    });
                };

                finish(async () => {
                    const buf = await blob.arrayBuffer();
                    if (this._aborted) return;
                    this.result = convert(buf);
                    this.readyState = DONE;
                    this._fire('progress', { loaded: size, total: size });
                    this._fire('load', { loaded: size, total: size });
                    this._fire('loadend', { loaded: size, total: size });
                });
            }

            readAsArrayBuffer(blob) { this._read(blob, (buf) => buf); }

            readAsText(blob, encoding) {
                const enc = encoding || 'utf-8';
                this._read(blob, (buf) => new TextDecoder(enc).decode(new Uint8Array(buf)));
            }

            readAsBinaryString(blob) {
                this._read(blob, (buf) => {
                    const bytes = new Uint8Array(buf);
                    let s = '';
                    // Chunked: String.fromCharCode.apply with a whole model's
                    // worth of bytes overflows the argument list.
                    for (let i = 0; i < bytes.length; i += 8192)
                        s += String.fromCharCode.apply(null, bytes.subarray(i, i + 8192));
                    return s;
                });
            }

            readAsDataURL(blob) {
                const type = (blob && blob.type) || 'application/octet-stream';
                this._read(blob, (buf) => {
                    const b64 = bytesToBase64(new Uint8Array(buf));
                    return 'data:' + type + ';base64,' + b64;
                });
            }

            abort() {
                if (this.readyState !== LOADING) return;
                this._aborted = true;
                this.readyState = DONE;
                this.result = null;
                this._fire('abort', { loaded: 0, total: 0 });
                this._fire('loadend', { loaded: 0, total: 0 });
            }
        }

        FileReader.EMPTY = EMPTY;
        FileReader.LOADING = LOADING;
        FileReader.DONE = DONE;
        FileReader.prototype.EMPTY = EMPTY;
        FileReader.prototype.LOADING = LOADING;
        FileReader.prototype.DONE = DONE;

        const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
        function bytesToBase64(bytes) {
            let out = '';
            let i = 0;
            for (; i + 2 < bytes.length; i += 3) {
                const n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
                out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] +
                       B64[(n >> 6) & 63] + B64[n & 63];
            }
            const rem = bytes.length - i;
            if (rem === 1) {
                const n = bytes[i] << 16;
                out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + '==';
            } else if (rem === 2) {
                const n = (bytes[i] << 16) | (bytes[i + 1] << 8);
                out += B64[(n >> 18) & 63] + B64[(n >> 12) & 63] + B64[(n >> 6) & 63] + '=';
            }
            return out;
        }

        globalThis.FileReader = FileReader;
    }

    // ------------------------------------------------------------- object URLs
    //
    // `URL.createObjectURL(blob)` names some bytes so a URL consumer can read
    // them. brokit's version keeps the Blob in a JS Map, which serves JS-side
    // consumers and nothing else — so the URL resolved nowhere: `<img src>` saw
    // a path that did not exist, and `fetch()` tried to open a file called
    // "blob:...". Any app importing an asset it holds in memory — a decoded
    // texture, a model's embedded images, a generated frame — got silence.
    //
    // The C++ half copies the bytes into a process-global table the native
    // consumers read (see js/dom_bindings.cpp); the Map here keeps the Blob
    // itself alive so fetch can answer with the very object the page passed in.
    if (typeof globalThis.__bro_createObjectURL === 'function' &&
        typeof globalThis.URL === 'function') {
        const nativeRevoke = URL.revokeObjectURL;
        const registry = new Map();

        URL.createObjectURL = function (obj) {
            const url = __bro_createObjectURL(obj);
            if (url === null)
                throw new TypeError(
                    'URL.createObjectURL: argument is not a Blob, File or MediaSource');
            registry.set(url, obj);
            return url;
        };

        URL.revokeObjectURL = function (url) {
            registry.delete(url);
            __bro_revokeObjectURL(url);
            // brokit minted URLs of its own before this point; let its
            // registry drop them too rather than leaking one per page.
            if (typeof nativeRevoke === 'function') nativeRevoke.call(URL, url);
        };

        // fetch(objectURL). Loaders reach for fetch for anything that isn't an
        // image — a glTF's .bin buffer, a shader, a JSON side-car — and the URL
        // they are handed for an in-memory asset is an object URL.
        const nativeFetch = globalThis.fetch;
        if (typeof nativeFetch === 'function' && typeof Response === 'function') {
            globalThis.fetch = function (input, init) {
                const url = (typeof input === 'string') ? input
                          : (input && typeof input.url === 'string') ? input.url : null;
                if (url !== null && url.lastIndexOf('blob:', 0) === 0) {
                    const blob = registry.get(url);
                    if (!blob)
                        return Promise.reject(new TypeError(
                            'Failed to fetch: ' + url + ' is not a live object URL'));
                    const res = new Response(blob, {
                        status: 200,
                        headers: { 'Content-Type': blob.type || 'application/octet-stream',
                                   'Content-Length': String(blob.size) },
                    });
                    res.url = url;
                    return Promise.resolve(res);
                }
                return nativeFetch.apply(this, arguments);
            };
        }
    }

    // ------------------------------------------------- dropped path → real File
    //
    // A drop hands the engine OS paths. What the page expects in
    // `dataTransfer.files` is File objects: things with a size, a type, and
    // bytes that `FileReader`, `URL.createObjectURL` and `fetch` can all read.
    // The bytes are read here, eagerly, because that is what makes the File a
    // real one — a lazily-filled Blob reads as empty to every native consumer,
    // and `URL.createObjectURL(file)` (how three.js resolves a model's textures)
    // would hand back an empty resource.
    const MIME = {
        png: 'image/png',   jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif',
        webp: 'image/webp', bmp: 'image/bmp',  svg: 'image/svg+xml',
        json: 'application/json', js: 'text/javascript', mjs: 'text/javascript',
        css: 'text/css',    html: 'text/html', txt: 'text/plain',  md: 'text/plain',
        wav: 'audio/wav',   mp3: 'audio/mpeg', ogg: 'audio/ogg',   webm: 'video/webm',
        mp4: 'video/mp4',   glb: 'model/gltf-binary', gltf: 'model/gltf+json',
        zip: 'application/zip', wasm: 'application/wasm',
    };

    globalThis.__bro_fileFromPath = function (path) {
        try {
            const fs = require('fs');
            const bytes = fs.readFileSync(path);
            const name = String(path).split(/[\\/]/).pop();
            const ext = name.indexOf('.') >= 0 ? name.split('.').pop().toLowerCase() : '';
            let lastModified = 0;
            try { lastModified = Math.floor(fs.statSync(path).mtimeMs); } catch (e) {}
            const file = new File([bytes], name, {
                type: MIME[ext] || '',
                lastModified: lastModified,
            });
            // Where it came from. Not a web property — bro apps get real paths
            // (see docs/paths-api.js), and a dropped file is the one moment a
            // page is handed one, so keep it rather than making the app guess.
            Object.defineProperty(file, 'path', {
                value: String(path), enumerable: false, writable: false,
            });
            return file;
        } catch (e) {
            return null;
        }
    };
})();
