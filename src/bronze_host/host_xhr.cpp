// XMLHttpRequest for a bronze-compiled app: a text or binary asset off disk or
// off http(s), and named refusals for the rest.
//
// WHY XHR AND NOT fetch, since the brief left the choice open and the answer
// turned out to be forced:
//
//   * `fetch` returns a Promise, and the embed API (src/embed/embed.h) had NO
//     way to create or resolve a bronze Promise from C++ when this was
//     written - no makePromise, no resolver pair, and no route to the
//     intrinsic %Promise% constructor. A host `fetch` would therefore have had
//     to be a JS shim compiled into the app, which is the thing the brief
//     rules out. XMLHttpRequest settles through callbacks, so it needs nothing
//     bronze does not already give a host. (`ev::createPromise` exists now and
//     host_fetch.cpp uses it; this object stays because the callback shape is
//     still what the XHR-era loaders below want.)
//
//   * Nothing at this boundary is promise-shaped as a result: the completion
//     is a host task (postHostTask), the handlers are ordinary callbacks, and
//     any Promise the app wants is one the app builds around them.
//
// WHAT THIS DOES NOT UNBLOCK, said plainly so nobody plans around it: three.js
// r160's FileLoader is fetch-based, not XHR-based (the library moved off XHR
// in r117), so THIS DOES NOT make three's own loaders work. It is here for the
// loaders - three's older ones, and a great deal of userland code - that are
// still written against XHR.
//
// RESPONSE TYPES. '', 'text', 'arraybuffer', 'blob' and 'json' are all served:
// embed::createArrayBuffer builds what 'arraybuffer' wants, makeBlobValue what
// 'blob' wants, and embed::parseJson what 'json' wants. 'document' is the one
// gap left, and it is a gap rather than a limit - host_parser.cpp has the
// parser it would need - so it stays a named refusal on assignment rather than
// a request that silently answers null.
//
// TRANSPORT. Local files through the shared app-path rules (js/asset_path.h),
// plus http(s) through util::fetchRemoteCached - the same remote-asset path
// fetch uses, so the two agree about what a URL means and about what is
// cached.

#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"  // ObjectBuilder, argAt

#include "js/asset_path.h"
#include "util/object_url.h"
#include "util/remote_asset.h"
#include "util/log.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// readyState, the four values this layer can actually be in. HEADERS_RECEIVED
// and LOADING are not modelled: the read is one synchronous act, so there is no
// moment at which headers exist and the body does not.
constexpr int kUnsent = 0;
constexpr int kOpened = 1;
constexpr int kDone = 4;

struct HostXhr {
    uint32_t tag = kHostXhrTag;  // must be first — see host_internal.h
    std::string method;
    std::string url;
    std::string responseType;  // "", "text", "arraybuffer", "blob", "json"
    std::string responseText;
    std::string statusText;
    int status = 0;
    int readyState = kUnsent;
    bool ok = false;
    bool aborted = false;
};

void hostXhrDtor(void* p) { delete static_cast<HostXhr*>(p); }

HostXhr* xhrOf(Value v) {
    auto* xhr = static_cast<HostXhr*>(ev::handleData(v));
    if (!xhr || xhr->tag != kHostXhrTag) return nullptr;
    return xhr;
}

// Read the whole file, or report why not. Kept here rather than reached for
// through a util because the failure taxonomy is the HTTP one this object has
// to answer in: a missing file is a 404, a file that will not open is a 500,
// and neither is an exception.
bool readWholeFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buf;
    buf << in.rdbuf();
    out = buf.str();
    return true;
}

Value xhrOpen(Value thisValue, std::span<const Value> a) {
    ev::Persistent self(thisValue);
    Value methodV = argAt(a, 0);
    Value urlV = argAt(a, 1);
    if (ev::isObject(methodV) || ev::isObject(urlV)) {
        return ev::throwTypeError("XMLHttpRequest.open(method, url): both must be strings");
    }
    const std::string method = ev::toUtf8(methodV);
    const std::string url = ev::toUtf8(urlV);

    HostXhr* xhr = xhrOf(self.get());
    if (!xhr) return ev::throwTypeError("XMLHttpRequest.open: the receiver is not a request");
    xhr->method = method;
    xhr->url = url;
    xhr->readyState = kOpened;
    xhr->status = 0;
    xhr->statusText.clear();
    xhr->responseText.clear();
    xhr->ok = false;
    xhr->aborted = false;
    // The third argument is `async`. This transport is synchronous either way
    // and the EVENTS are asynchronous either way (they are host tasks), so a
    // request opened with async=false still settles on the next frame. That is
    // a real divergence from the web, and it is the one a host with a single
    // per-frame seam can honour - a synchronous fire would re-enter compiled
    // code from inside send().
    return ev::undefined();
}

Value xhrSend(Value thisValue, std::span<const Value>) {
    ev::Persistent self(thisValue);
    HostXhr* xhr = xhrOf(self.get());
    if (!xhr) return ev::throwTypeError("XMLHttpRequest.send: the receiver is not a request");
    if (xhr->readyState != kOpened) {
        return ev::throwError("XMLHttpRequest.send: open() must be called first");
    }

    if (!xhr->method.empty() && xhr->method != "GET" && xhr->method != "get") {
        // A write verb has no meaning against a read-only asset path, and
        // silently doing a GET instead is the kind of fallback that gets
        // debugged for an hour.
        LOG_ERROR("bronze_host: XMLHttpRequest only serves GET, got %s",
                  xhr->method.c_str());
        xhr->status = 405;
        xhr->statusText = "Method Not Allowed";
    } else if (xhr->url.rfind("http://", 0) == 0 || xhr->url.rfind("https://", 0) == 0) {
        std::string content = util::fetchRemoteCached(xhr->url);
        if (!content.empty()) {
            xhr->responseText = std::move(content);
            xhr->status = 200;
            xhr->statusText = "OK";
            xhr->ok = true;
        } else {
            xhr->status = 404;
            xhr->statusText = "Not Found";
            xhr->ok = false;
        }
    } else if (std::vector<uint8_t> inline_;
               util::inlineURLBytes(xhr->url, inline_)) {
        // A `blob:` or `data:` URL carries its own bytes. Ahead of
        // resolveAssetPath, which would read `blob:bro/7` as a filename under
        // the app directory and answer 404 for something that was never a file.
        xhr->responseText.assign(inline_.begin(), inline_.end());
        xhr->status = 200;
        xhr->statusText = "OK";
        xhr->ok = true;
    } else {
        const std::string path = js::resolveAssetPath(xhr->url);
        if (readWholeFile(path, xhr->responseText)) {
            xhr->status = 200;
            xhr->statusText = "OK";
            xhr->ok = true;
        } else {
            xhr->status = 404;
            xhr->statusText = "Not Found";
            LOG_WARN("bronze_host: XMLHttpRequest could not read %s", path.c_str());
        }
    }

    ev::Persistent target(self.get());
    postHostTask([target]() {
        // The read already happened; this is only the notification, on the
        // frame seam where re-entering compiled code is expected.
        ev::Persistent local(target);
        HostXhr* state = xhrOf(local.get());
        if (!state || state->aborted) return;
        state->readyState = kDone;
        dispatchHostEvent(local, "readystatechange");
        dispatchHostEvent(local, state->ok ? "load" : "error");
        dispatchHostEvent(local, "loadend");
    });
    return ev::undefined();
}

Value xhrAbort(Value thisValue, std::span<const Value>) {
    HostXhr* xhr = xhrOf(thisValue);
    if (!xhr) return ev::undefined();
    // The queued completion checks this flag and does nothing, which is the
    // whole of abort here: the transfer already finished, so there is nothing
    // in flight to cancel - only an event to suppress.
    xhr->aborted = true;
    xhr->readyState = kUnsent;
    xhr->status = 0;
    xhr->responseText.clear();
    return ev::undefined();
}

HostClass g_xhrClass;

Value makeXhrValue() {
    auto* xhr = new HostXhr();
    ObjectBuilder b(g_xhrClass.make(xhr, hostXhrDtor));

    // Handler slots, present and null so an assignment writes a data property
    // that is already in the shape.
    for (const char* name : {"onload", "onerror", "onabort", "onprogress",
                             "onloadend", "onreadystatechange", "ontimeout"}) {
        Value nul = ev::null();
        b.set(name, nul);
    }
    // Accepted and ignored: there is no origin and no timer on a file read.
    {
        Value no = ev::fromBool(false);
        b.set("withCredentials", no);
    }
    {
        Value zero = ev::fromDouble(0);
        b.set("timeout", zero);
    }

    return b.get();
}

// Everything an XMLHttpRequest can DO or REPORT, decorated once onto
// XMLHttpRequest.prototype. Every one of these already read its receiver, so
// moving them off the instance changed nothing but the number of copies.
void decorateXhrProto(ObjectBuilder& b) {
    // Every piece of live state is an accessor over the payload, so the program
    // cannot desync `status` from what actually happened by assigning to it.
    b.accessor("readyState",
               [](Value thisValue, std::span<const Value>) {
                   const HostXhr* s = xhrOf(thisValue);
                   return ev::fromDouble(s ? s->readyState : kUnsent);
               },
               nullptr);
    b.accessor("status",
               [](Value thisValue, std::span<const Value>) {
                   const HostXhr* s = xhrOf(thisValue);
                   return ev::fromDouble(s ? s->status : 0);
               },
               nullptr);
    b.accessor("statusText",
               [](Value thisValue, std::span<const Value>) {
                   const HostXhr* s = xhrOf(thisValue);
                   return ev::fromUtf8(s ? s->statusText : std::string());
               },
               nullptr);
    b.accessor("responseURL",
               [](Value thisValue, std::span<const Value>) {
                   const HostXhr* s = xhrOf(thisValue);
                   return ev::fromUtf8(s ? s->url : std::string());
               },
               nullptr);
    b.accessor("responseText",
               [](Value thisValue, std::span<const Value>) {
                   const HostXhr* s = xhrOf(thisValue);
                   if (!s) return ev::fromUtf8("");
                   if (!s->responseType.empty() && s->responseType != "text") {
                       return ev::throwError(
                           "InvalidStateError: responseText is only available when responseType is '' or 'text'");
                   }
                   return ev::fromUtf8(s->responseText);
               },
               nullptr);
    // `response` answers the empty STRING, not null, for the text types — that
    // is what the spec says for a request that is not done, and it is what a
    // program written against the web reads when a request fails. The binary
    // and json types answer null in the same state, which is also the spec: the
    // difference is that "" is a valid text response and there is no valid
    // empty ArrayBuffer answer to give.
    b.accessor("response",
               [](Value thisValue, std::span<const Value>) {
                   const HostXhr* s = xhrOf(thisValue);
                   if (!s) return ev::null();
                   if (s->responseType.empty() || s->responseType == "text") {
                       return ev::fromUtf8(s->readyState == kDone ? s->responseText
                                                                 : std::string());
                   }
                   if (s->readyState != kDone || !s->ok) return ev::null();
                   if (s->responseType == "arraybuffer") {
                       return ev::createArrayBuffer(std::span<const uint8_t>(
                           reinterpret_cast<const uint8_t*>(s->responseText.data()),
                           s->responseText.size()));
                   }
                   if (s->responseType == "blob") {
                       std::vector<uint8_t> bytes(s->responseText.begin(), s->responseText.end());
                       return makeBlobValue(std::move(bytes), "application/octet-stream");
                   }
                   if (s->responseType == "json") {
                       ev::CallResult r = ev::parseJson(s->responseText);
                       if (r.thrown) return ev::null();
                       return r.value;
                   }
                   return ev::null();
               },
               nullptr);
    b.accessor("responseType",
               [](Value thisValue, std::span<const Value>) {
                   const HostXhr* s = xhrOf(thisValue);
                   return ev::fromUtf8(s ? s->responseType : std::string());
               },
               [](Value thisValue, std::span<const Value> a) {
                   Value v = argAt(a, 0);
                   if (ev::isObject(v)) return ev::undefined();
                   const std::string want = ev::toUtf8(v);
                   HostXhr* s = xhrOf(thisValue);
                   if (!s) return ev::undefined();
                   if (want.empty() || want == "text" || want == "arraybuffer" ||
                       want == "blob" || want == "json") {
                       s->responseType = want;
                       return ev::undefined();
                   }
                   return ev::throwTypeError(
                       "XMLHttpRequest.responseType '" + want +
                       "' is not supported; only '', 'text', 'arraybuffer', 'blob', 'json' are");
               });

    b.def("open", 3, xhrOpen);
    b.def("send", 1, xhrSend);
    b.def("abort", 0, xhrAbort);
    b.def("setRequestHeader", 2, [](Value, std::span<const Value>) {
        // Accepted and dropped: a file read has no request headers to carry
        // them. Refusing would break every loader that sets Accept.
        return ev::undefined();
    });
    b.def("overrideMimeType", 1, [](Value, std::span<const Value>) {
        return ev::undefined();
    });
    b.def("getResponseHeader", 1, [](Value, std::span<const Value>) {
        // No response headers exist, and null is what the web answers for a
        // header that is absent - so this is the honest answer, not a stub.
        return ev::null();
    });
    b.def("getAllResponseHeaders", 0, [](Value, std::span<const Value>) {
        return ev::fromUtf8("");
    });

    b.def("addEventListener", 2, [](Value thisValue, std::span<const Value> a) {
        ev::Persistent self(thisValue);
        Value typeV = argAt(a, 0);
        if (ev::isObject(typeV)) return ev::undefined();
        const std::string type = ev::toUtf8(typeV);
        addHostListener(self, type, argAt(a, 1));
        return ev::undefined();
    });
    b.def("removeEventListener", 2, [](Value thisValue, std::span<const Value> a) {
        ev::Persistent self(thisValue);
        Value typeV = argAt(a, 0);
        if (ev::isObject(typeV)) return ev::undefined();
        const std::string type = ev::toUtf8(typeV);
        removeHostListener(self, type, argAt(a, 1));
        return ev::undefined();
    });
}

}  // namespace

void installXhrGlobal() {
    // Same construct story as Image (host_image.cpp): bronze_construct replaces
    // the plain instance with the object the body returns, so `new
    // XMLHttpRequest()` and `XMLHttpRequest()` answer the same thing - and
    // since the instance is born on this constructor's prototype, `instanceof`
    // answers true.
    g_xhrClass.install(
        "XMLHttpRequest", 0,
        [](Value, std::span<const Value>) { return makeXhrValue(); },
        decorateXhrProto);
    // The readyState constants, where the web has them.
    g_xhrClass.setStatic("UNSENT", ev::fromDouble(0));
    g_xhrClass.setStatic("OPENED", ev::fromDouble(1));
    g_xhrClass.setStatic("HEADERS_RECEIVED", ev::fromDouble(2));
    g_xhrClass.setStatic("LOADING", ev::fromDouble(3));
    g_xhrClass.setStatic("DONE", ev::fromDouble(4));
}

}  // namespace bro::bronze_host
