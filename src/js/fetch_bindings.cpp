#include "js/fetch_bindings.h"
#include "util/log.h"

#include <quickjs.h>
#include <fstream>
#include <string>
#include <vector>
#include <cstring>

namespace bro::js {

static std::string s_fetchBasePath;

static std::string resolveUrl(const std::string& url) {
    // Strip leading ./
    std::string clean = url;
    if (clean.size() >= 2 && clean[0] == '.' && clean[1] == '/') {
        clean = clean.substr(2);
    }
    // Already absolute?
    if (clean.size() >= 2 && clean[1] == ':') return clean;
    if (!clean.empty() && (clean[0] == '/' || clean[0] == '\\')) return clean;
    // Relative — join with base
    if (s_fetchBasePath.empty()) return clean;
    std::string path = s_fetchBasePath;
    if (path.back() != '/' && path.back() != '\\') path += '/';
    return path + clean;
}

// __bro_readFile(path) -> ArrayBuffer | null
static JSValue js_bro_readFile(JSContext* ctx, JSValueConst /*this_val*/,
                                int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    const char* url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_NULL;

    std::string urlStr(url);
    std::string path = resolveUrl(urlStr);
    JS_FreeCString(ctx, url);

    std::ifstream file(path, std::ios::in | std::ios::binary | std::ios::ate);
    if (!file) {
        LOG_WARN("fetch: file not found: '%s' (from URL '%s')", path.c_str(), urlStr.c_str());
        return JS_NULL;
    }

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data((size_t)size);
    file.read(reinterpret_cast<char*>(data.data()), size);

    return JS_NewArrayBufferCopy(ctx, data.data(), data.size());
}

// The JS polyfill that builds fetch/Response/Headers/TextDecoder on top of __bro_readFile
static const char* FETCH_POLYFILL = R"JS(
(function() {
    // --- TextDecoder ---
    if (typeof globalThis.TextDecoder === 'undefined') {
        globalThis.TextDecoder = function TextDecoder(encoding) {
            this.encoding = (encoding || 'utf-8').toLowerCase();
        };
        globalThis.TextDecoder.prototype.decode = function(input) {
            if (!input) return '';
            var bytes;
            if (input instanceof ArrayBuffer) {
                bytes = new Uint8Array(input);
            } else if (input instanceof Uint8Array) {
                bytes = input;
            } else if (ArrayBuffer.isView(input)) {
                bytes = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
            } else {
                return '';
            }
            // Fast path: build string from char codes in chunks
            var result = '';
            var len = bytes.length;
            for (var i = 0; i < len; i += 4096) {
                var end = i + 4096;
                if (end > len) end = len;
                var chunk = '';
                for (var j = i; j < end; j++) {
                    chunk += String.fromCharCode(bytes[j]);
                }
                result += chunk;
            }
            return result;
        };
    }

    // --- TextEncoder ---
    if (typeof globalThis.TextEncoder === 'undefined') {
        globalThis.TextEncoder = function TextEncoder() {
            this.encoding = 'utf-8';
        };
        globalThis.TextEncoder.prototype.encode = function(str) {
            str = str || '';
            var arr = [];
            for (var i = 0; i < str.length; i++) {
                var c = str.charCodeAt(i);
                if (c < 128) {
                    arr.push(c);
                } else if (c < 2048) {
                    arr.push(0xC0 | (c >> 6), 0x80 | (c & 0x3F));
                } else {
                    arr.push(0xE0 | (c >> 12), 0x80 | ((c >> 6) & 0x3F), 0x80 | (c & 0x3F));
                }
            }
            return new Uint8Array(arr);
        };
    }

    // --- Headers ---
    if (typeof globalThis.Headers === 'undefined') {
        globalThis.Headers = function Headers(init) {
            this._map = {};
            if (init && typeof init === 'object') {
                var keys = Object.keys(init);
                for (var i = 0; i < keys.length; i++) {
                    this._map[keys[i].toLowerCase()] = String(init[keys[i]]);
                }
            }
        };
        Headers.prototype.get = function(name) {
            return this._map[name.toLowerCase()] || null;
        };
        Headers.prototype.set = function(name, value) {
            this._map[name.toLowerCase()] = String(value);
        };
        Headers.prototype.has = function(name) {
            return name.toLowerCase() in this._map;
        };
    }

    // --- Request ---
    if (typeof globalThis.Request === 'undefined') {
        globalThis.Request = function Request(input, init) {
            if (typeof input === 'string') {
                this.url = input;
            } else if (input && input.url) {
                this.url = input.url;
            } else {
                this.url = String(input);
            }
            init = init || {};
            this.method = init.method || 'GET';
            this.headers = init.headers instanceof Headers ? init.headers : new Headers(init.headers);
            this.credentials = init.credentials || 'same-origin';
            this.signal = init.signal || null;
        };
    }

    // --- Response ---
    function BroResponse(data, url) {
        this._data = data;  // ArrayBuffer or null
        this.ok = data !== null;
        this.status = data !== null ? 200 : 404;
        this.statusText = data !== null ? 'OK' : 'Not Found';
        this.url = url || '';
        this.type = 'basic';
        var h = {};
        if (data) {
            h['content-length'] = String(data.byteLength);
            h['content-type'] = 'application/octet-stream';
        }
        this.headers = new Headers(h);
        this.body = this._makeBody();
    }

    BroResponse.prototype._makeBody = function() {
        var data = this._data;
        return {
            getReader: function() {
                var done = false;
                return {
                    read: function() {
                        if (done || !data) {
                            return Promise.resolve({ done: true, value: undefined });
                        }
                        done = true;
                        return Promise.resolve({ done: false, value: new Uint8Array(data) });
                    },
                    cancel: function() { done = true; return Promise.resolve(); },
                    releaseLock: function() {}
                };
            }
        };
    };

    BroResponse.prototype.arrayBuffer = function() {
        if (!this._data) return Promise.reject(new Error('No data'));
        return Promise.resolve(this._data.slice(0));
    };

    BroResponse.prototype.text = function() {
        if (!this._data) return Promise.reject(new Error('No data'));
        var decoder = new TextDecoder();
        return Promise.resolve(decoder.decode(this._data));
    };

    BroResponse.prototype.json = function() {
        return this.text().then(function(t) { return JSON.parse(t); });
    };

    BroResponse.prototype.blob = function() {
        // Minimal Blob shim — just wraps the ArrayBuffer
        if (!this._data) return Promise.reject(new Error('No data'));
        var data = this._data;
        return Promise.resolve({
            size: data.byteLength,
            type: 'application/octet-stream',
            arrayBuffer: function() { return Promise.resolve(data.slice(0)); },
            text: function() { return Promise.resolve(new TextDecoder().decode(data)); }
        });
    };

    BroResponse.prototype.clone = function() {
        var cloned = new BroResponse(this._data ? this._data.slice(0) : null, this.url);
        return cloned;
    };

    // --- fetch ---
    globalThis.fetch = function fetch(input, init) {
        var url;
        if (typeof input === 'string') {
            url = input;
        } else if (input && input.url) {
            url = input.url;
        } else {
            url = String(input);
        }

        return new Promise(function(resolve, reject) {
            try {
                var data = globalThis.__bro_readFile(url);
                resolve(new BroResponse(data, url));
            } catch (e) {
                reject(e);
            }
        });
    };

    // --- AbortController / AbortSignal stubs ---
    if (typeof globalThis.AbortController === 'undefined') {
        globalThis.AbortController = function AbortController() {
            this.signal = { aborted: false, addEventListener: function() {}, removeEventListener: function() {} };
        };
        AbortController.prototype.abort = function() { this.signal.aborted = true; };
    }

    // --- ProgressEvent stub ---
    if (typeof globalThis.ProgressEvent === 'undefined') {
        globalThis.ProgressEvent = function ProgressEvent(type, init) {
            this.type = type;
            this.lengthComputable = (init && init.lengthComputable) || false;
            this.loaded = (init && init.loaded) || 0;
            this.total = (init && init.total) || 0;
        };
    }
})();
)JS";

void FetchBindings::install(JSContext* ctx, const std::string& basePath) {
    s_fetchBasePath = basePath;

    // Register native __bro_readFile
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__bro_readFile",
        JS_NewCFunction(ctx, js_bro_readFile, "__bro_readFile", 1));
    JS_FreeValue(ctx, global);

    // Evaluate the JS polyfill
    JSValue result = JS_Eval(ctx, FETCH_POLYFILL, strlen(FETCH_POLYFILL),
                             "<fetch-polyfill>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* str = JS_ToCString(ctx, exc);
        if (str) {
            LOG_ERROR("fetch polyfill failed: %s", str);
            JS_FreeCString(ctx, str);
        }
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, result);

    LOG_INFO("Fetch API installed (local file backend)");
}

} // namespace bro::js
