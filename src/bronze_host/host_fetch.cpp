// fetch() for a bronze-compiled app: reads over the engine's asset mounts,
// returning a real bronze Promise resolving to a minimal Response object.
//
// TRANSPORT. Local files only, through the shared app-path rules
// (js/asset_path.h). http(s) belongs to brokit; a bronze-side network client
// has no transport here and settles with status 0 and ok=false.
//
// RESPONSE. A minimal Response object providing:
//   - ok (boolean: true for status 200..299)
//   - status (number: 200 on success, 404/500 on fail)
//   - statusText (string)
//   - url (string)
//   - headers (Headers)
//   - text() -> Promise<string>
//   - json() -> Promise<any> (parsed via bronze runtime / embed mechanisms)
//   - arrayBuffer() -> Promise<ArrayBuffer>
//
// HEADERS. A real case-insensitive header map honoring get, set, has, append,
// and initialized from an optional Headers instance, sequence of pairs, or object.
//
// REQUEST. Carries url, method, and headers initialized from input and init.
// fetch accepts exactly a URL string or Request object.
//
// GC RULE. Payloads are plain host memory (std::vector<uint8_t>, std::string,
// std::map) owned by handle cells. They hold no heap references, so the handle
// finalizers free C++ memory mid-collection without calling into the embed API.

#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"  // ObjectBuilder, argAt

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/heap.h"
#include "runtime/value.h"

#include "js/asset_path.h"
#include "util/log.h"

#include <cctype>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace bro::bronze_host {

namespace {

std::string toLowerAscii(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Headers
// ---------------------------------------------------------------------------

struct HostHeaders {
    uint32_t tag = kHostHeadersTag;
    std::map<std::string, std::string> entries;
};

void hostHeadersDtor(void* p) { delete static_cast<HostHeaders*>(p); }

HostHeaders* headersOf(Value v) {
    auto* h = static_cast<HostHeaders*>(ev::handleData(v));
    if (!h || h->tag != kHostHeadersTag) return nullptr;
    return h;
}

Value headersGet(Value thisValue, std::span<const Value> a) {
    HostHeaders* h = headersOf(thisValue);
    if (!h) return ev::throwTypeError("Headers.get: receiver is not a Headers");
    if (a.empty() || ev::isUndefined(a[0]) || ev::isNull(a[0])) return ev::null();
    std::string key = toLowerAscii(ev::toUtf8(a[0]));
    auto it = h->entries.find(key);
    if (it == h->entries.end()) return ev::null();
    return ev::fromUtf8(it->second);
}

Value headersSet(Value thisValue, std::span<const Value> a) {
    HostHeaders* h = headersOf(thisValue);
    if (!h) return ev::throwTypeError("Headers.set: receiver is not a Headers");
    if (a.empty() || ev::isUndefined(a[0]) || ev::isNull(a[0])) {
        return ev::throwTypeError("Headers.set: name is required");
    }
    std::string key = toLowerAscii(ev::toUtf8(a[0]));
    Value valV = argAt(a, 1);
    std::string val = (!ev::isUndefined(valV) && !ev::isNull(valV)) ? ev::toUtf8(valV) : "";
    h->entries[key] = val;
    return ev::undefined();
}

Value headersHas(Value thisValue, std::span<const Value> a) {
    HostHeaders* h = headersOf(thisValue);
    if (!h) return ev::throwTypeError("Headers.has: receiver is not a Headers");
    if (a.empty() || ev::isUndefined(a[0]) || ev::isNull(a[0])) return ev::fromBool(false);
    std::string key = toLowerAscii(ev::toUtf8(a[0]));
    return ev::fromBool(h->entries.find(key) != h->entries.end());
}

Value headersAppend(Value thisValue, std::span<const Value> a) {
    HostHeaders* h = headersOf(thisValue);
    if (!h) return ev::throwTypeError("Headers.append: receiver is not a Headers");
    if (a.empty() || ev::isUndefined(a[0]) || ev::isNull(a[0])) {
        return ev::throwTypeError("Headers.append: name is required");
    }
    std::string key = toLowerAscii(ev::toUtf8(a[0]));
    Value valV = argAt(a, 1);
    std::string val = (!ev::isUndefined(valV) && !ev::isNull(valV)) ? ev::toUtf8(valV) : "";
    auto it = h->entries.find(key);
    if (it == h->entries.end()) {
        h->entries[key] = val;
    } else {
        it->second += ", " + val;
    }
    return ev::undefined();
}

Value makeHeadersValue(HostHeaders* h) {
    ObjectBuilder b(ev::makeHandle(h, hostHeadersDtor));
    b.def("get", 1, headersGet);
    b.def("set", 2, headersSet);
    b.def("has", 1, headersHas);
    b.def("append", 2, headersAppend);
    return b.get();
}

void initHeadersFromValue(HostHeaders* self, Value initV) {
    if (ev::isUndefined(initV) || ev::isNull(initV)) return;

    if (auto* other = headersOf(initV)) {
        self->entries = other->entries;
        return;
    }

    if (!ev::isObject(initV)) return;

    ev::Persistent initRoot(initV);
    uint64_t keysBits = bronze_object_keys(initV.rawBits());
    Value keysVal(keysBits);
    if (keysVal.isObject()) {
        auto* keysArr = keysVal.asObject<bronze::ArrayHeader>();
        uint32_t len = keysArr->length;
        bool isArray = initV.asObject<bronze::HeapObjectHeader>()->flags == bronze::HeapKind::Array;
        if (isArray) {
            for (uint32_t i = 0; i < len; ++i) {
                Value pair = ev::getElement(initRoot.get(), i);
                if (ev::isObject(pair)) {
                    Value kVal = ev::getElement(pair, 0);
                    Value vVal = ev::getElement(pair, 1);
                    if (!ev::isUndefined(kVal) && !ev::isNull(kVal)) {
                        std::string kStr = toLowerAscii(ev::toUtf8(kVal));
                        std::string vStr = (!ev::isUndefined(vVal) && !ev::isNull(vVal)) ? ev::toUtf8(vVal) : "";
                        auto it = self->entries.find(kStr);
                        if (it == self->entries.end()) {
                            self->entries[kStr] = vStr;
                        } else {
                            it->second += ", " + vStr;
                        }
                    }
                }
            }
        } else {
            for (uint32_t i = 0; i < len; ++i) {
                Value kVal = keysVal.asObject<bronze::ArrayHeader>()->getElem(i);
                std::string kStr = ev::toUtf8(kVal);
                Value vVal = ev::getProperty(initRoot.get(), kStr);
                std::string vStr = (!ev::isUndefined(vVal) && !ev::isNull(vVal)) ? ev::toUtf8(vVal) : "";
                self->entries[toLowerAscii(kStr)] = vStr;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Request
// ---------------------------------------------------------------------------

struct HostRequest {
    uint32_t tag = kHostRequestTag;
    std::string url;
    std::string method = "GET";
    HostHeaders headers;
};

void hostRequestDtor(void* p) { delete static_cast<HostRequest*>(p); }

HostRequest* requestOf(Value v) {
    auto* req = static_cast<HostRequest*>(ev::handleData(v));
    if (!req || req->tag != kHostRequestTag) return nullptr;
    return req;
}

Value makeRequestValue(HostRequest* req) {
    ObjectBuilder b(ev::makeHandle(req, hostRequestDtor));
    b.set("url", ev::fromUtf8(req->url));
    b.set("method", ev::fromUtf8(req->method));

    auto* h = new HostHeaders(req->headers);
    b.set("headers", makeHeadersValue(h));

    return b.get();
}

Value requestCtor(Value, std::span<const Value> a) {
    Value inputV = argAt(a, 0);
    if (ev::isUndefined(inputV) || ev::isNull(inputV)) {
        return ev::throwTypeError("Request: input is required");
    }

    auto* req = new HostRequest();
    if (auto* srcReq = requestOf(inputV)) {
        req->url = srcReq->url;
        req->method = srcReq->method;
        req->headers = srcReq->headers;
    } else if (!ev::isObject(inputV)) {
        req->url = ev::toUtf8(inputV);
        req->method = "GET";
    } else {
        delete req;
        return ev::throwTypeError("Request: input must be a URL string or Request object");
    }

    if (a.size() > 1 && !ev::isUndefined(a[1]) && !ev::isNull(a[1])) {
        Value initV = a[1];
        if (ev::isObject(initV)) {
            Value methodV = ev::getProperty(initV, "method");
            if (!ev::isUndefined(methodV) && !ev::isNull(methodV)) {
                std::string m = ev::toUtf8(methodV);
                for (char& c : m) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                req->method = m;
            }
            Value headersV = ev::getProperty(initV, "headers");
            if (!ev::isUndefined(headersV) && !ev::isNull(headersV)) {
                initHeadersFromValue(&req->headers, headersV);
            }
        }
    }

    return makeRequestValue(req);
}

// ---------------------------------------------------------------------------
// Response
// ---------------------------------------------------------------------------

struct HostResponse {
    uint32_t tag = kHostFetchTag;
    int status = 0;
    bool ok = false;
    std::string url;
    std::string statusText;
    std::vector<uint8_t> body;
};

void hostResponseDtor(void* p) { delete static_cast<HostResponse*>(p); }

HostResponse* responseOf(Value v) {
    auto* resp = static_cast<HostResponse*>(ev::handleData(v));
    if (!resp || resp->tag != kHostFetchTag) return nullptr;
    return resp;
}

Value responseText(Value thisValue, std::span<const Value>) {
    HostResponse* r = responseOf(thisValue);
    if (!r) return ev::throwTypeError("Response.text: receiver is not a Response");
    ev::Persistent p{ev::createPromise()};
    std::string str(reinterpret_cast<const char*>(r->body.data()), r->body.size());
    ev::resolvePromise(p.get(), ev::fromUtf8(str));
    return p.get();
}

Value responseJson(Value thisValue, std::span<const Value>) {
    HostResponse* r = responseOf(thisValue);
    if (!r) return ev::throwTypeError("Response.json: receiver is not a Response");
    ev::Persistent p{ev::createPromise()};
    std::string_view jsonStr(reinterpret_cast<const char*>(r->body.data()), r->body.size());
    ev::CallResult parsed = ev::parseJson(jsonStr);
    if (parsed.thrown) {
        ev::rejectPromise(p.get(), parsed.value);
    } else {
        ev::resolvePromise(p.get(), parsed.value);
    }
    return p.get();
}

Value responseArrayBuffer(Value thisValue, std::span<const Value>) {
    HostResponse* r = responseOf(thisValue);
    if (!r) return ev::throwTypeError("Response.arrayBuffer: receiver is not a Response");
    ev::Persistent p{ev::createPromise()};
    Value ab = ev::createArrayBuffer(std::span<const uint8_t>(r->body));
    ev::resolvePromise(p.get(), ab);
    return p.get();
}

Value makeResponseValue(HostResponse* resp) {
    ObjectBuilder b(ev::makeHandle(resp, hostResponseDtor));

    b.set("ok", ev::fromBool(resp->ok));
    b.set("status", ev::fromDouble(resp->status));
    b.set("statusText", ev::fromUtf8(resp->statusText));
    b.set("url", ev::fromUtf8(resp->url));

    auto* h = new HostHeaders();
    b.set("headers", makeHeadersValue(h));

    b.def("text", 0, responseText);
    b.def("json", 0, responseJson);
    b.def("arrayBuffer", 0, responseArrayBuffer);

    return b.get();
}

// ---------------------------------------------------------------------------
// fetch
// ---------------------------------------------------------------------------

Value fetchCall(Value, std::span<const Value> a) {
    Value inputV = argAt(a, 0);
    if (ev::isUndefined(inputV) || ev::isNull(inputV)) {
        return ev::throwTypeError("fetch: URL is required");
    }

    std::string url;
    std::string method = "GET";
    HostHeaders headers;

    if (auto* req = requestOf(inputV)) {
        url = req->url;
        method = req->method;
        headers = req->headers;
    } else if (!ev::isObject(inputV)) {
        url = ev::toUtf8(inputV);
    } else {
        return ev::throwTypeError("fetch: input must be a URL string or Request object");
    }

    if (a.size() > 1 && !ev::isUndefined(a[1]) && !ev::isNull(a[1])) {
        Value initV = a[1];
        if (ev::isObject(initV)) {
            Value methodV = ev::getProperty(initV, "method");
            if (!ev::isUndefined(methodV) && !ev::isNull(methodV)) {
                std::string m = ev::toUtf8(methodV);
                for (char& c : m) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                method = m;
            }
            Value headersV = ev::getProperty(initV, "headers");
            if (!ev::isUndefined(headersV) && !ev::isNull(headersV)) {
                initHeadersFromValue(&headers, headersV);
            }
        }
    }

    ev::Persistent promise{ev::createPromise()};
    ev::Persistent targetPromise(promise.get());

    postHostTask([targetPromise, url]() {
        ev::Persistent p(targetPromise);
        auto* resp = new HostResponse();
        resp->url = url;

        if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
            LOG_ERROR("bronze_host: fetch has no network transport; %s needs one",
                      url.c_str());
            resp->status = 0;
            resp->statusText = "No transport";
            resp->ok = false;
        } else {
            const std::string path = js::resolveAssetPath(url);
            std::ifstream in(path, std::ios::binary);
            if (in) {
                in.seekg(0, std::ios::end);
                auto sz = in.tellg();
                in.seekg(0, std::ios::beg);
                if (sz > 0) {
                    resp->body.resize(static_cast<size_t>(sz));
                    in.read(reinterpret_cast<char*>(resp->body.data()), sz);
                }
                resp->status = 200;
                resp->statusText = "OK";
                resp->ok = true;
            } else {
                resp->status = 404;
                resp->statusText = "Not Found";
                resp->ok = false;
                LOG_WARN("bronze_host: fetch could not read %s", path.c_str());
            }
        }

        Value respObj = makeResponseValue(resp);
        ev::resolvePromise(p.get(), respObj);
    });

    return promise.get();
}

}  // namespace

void installFetchGlobal() {
    Value fetchFn = ev::makeFunction(fetchCall, 1);
    ev::registerGlobal("fetch", fetchFn);

    Value reqFn = ev::makeFunction(requestCtor, 1);
    ev::registerGlobal("Request", reqFn);

    Value headersFn = ev::makeFunction(
        [](Value, std::span<const Value> a) {
            auto* h = new HostHeaders();
            if (!a.empty()) {
                initHeadersFromValue(h, a[0]);
            }
            return makeHeadersValue(h);
        },
        0);
    ev::registerGlobal("Headers", headersFn);

    Value respFn = ev::makeFunction(
        [](Value, std::span<const Value>) {
            auto* resp = new HostResponse();
            resp->status = 200;
            resp->statusText = "OK";
            resp->ok = true;
            return makeResponseValue(resp);
        },
        0);
    ev::registerGlobal("Response", respFn);
}

}  // namespace bro::bronze_host
