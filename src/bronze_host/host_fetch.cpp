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
//   - text() -> Promise<string>
//   - json() -> Promise<any> (parsed via bronze runtime / embed mechanisms)
//   - arrayBuffer() -> Promise<ArrayBuffer>
//
// GC RULE. The Response payload is plain host memory (std::vector<uint8_t>,
// std::string) owned by the handle cell. It holds no heap references, so the
// handle finalizer frees C++ memory mid-collection without calling into the
// embed API.

#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"  // ObjectBuilder, argAt

#include "js/asset_path.h"
#include "util/log.h"

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace bro::bronze_host {

namespace {

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

    b.def("text", 0, responseText);
    b.def("json", 0, responseJson);
    b.def("arrayBuffer", 0, responseArrayBuffer);

    return b.get();
}

Value fetchCall(Value, std::span<const Value> a) {
    Value urlV = argAt(a, 0);
    if (ev::isUndefined(urlV) || ev::isNull(urlV)) {
        return ev::throwTypeError("fetch: URL is required");
    }
    const std::string url = ev::toUtf8(urlV);

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
}

}  // namespace bro::bronze_host
