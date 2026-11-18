// document.fonts (CSS Font Loading 3 §5, FontFaceSet), for what pages ask of
// it: `document.fonts.ready.then(start)`, `document.fonts.load(font)` before
// measuring text, `check(font)`, and iterating the faces for their status.
//
// The page's @font-face faces load while its document loads
// (Engine::loadCustomFonts), before a script can ask, so the set is always
// settled: `status` "loaded", `ready` and `load()` already resolved. A face
// whose sources all failed iterates with status "error". FontFace objects
// made by script (`new FontFace`) are not supported, so add/delete change
// nothing.

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_list_iteration.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"

#include "dom/document.h"
#include "engine/engine.h"

#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// Promise.resolve(v).
Value resolvedPromise(Value vIn) {
    ev::Persistent v(vIn);
    ev::GlobalValue promise = ev::globalValue("Promise");
    if (!promise.found) return ev::undefined();
    ev::Persistent ctor(promise.value);
    ev::Persistent resolve(ev::getProperty(ctor.get(), "resolve"));
    const Value arg = v.get();
    ev::CallResult r = ev::call(resolve.get(), ctor.get(), std::span<const Value>(&arg, 1));
    return r.thrown ? ev::undefined() : r.value;
}

struct FaceInfo {
    std::string family;
    int weight;
    bool italic;
    bool loaded;
};

std::vector<FaceInfo> facesOf(dom::Document* doc) {
    std::vector<FaceInfo> out;
    if (!doc) return out;
    engine::Engine* eng = hostEngine();
    for (const auto& ff : doc->cascade().fontFaces()) {
        bool loaded = false;
        if (eng) {
            for (const auto& lf : eng->loadedFonts()) {
                if (lf.family == ff.family && lf.weight == ff.weight && lf.italic == ff.italic) loaded = true;
            }
        }
        out.push_back({ff.family, ff.weight, ff.italic, loaded});
    }
    return out;
}

Value makeFontFace(const FaceInfo& f) {
    ObjectBuilder b;
    b.set("family", ev::fromUtf8(f.family));
    b.set("style", ev::fromUtf8(f.italic ? "italic" : "normal"));
    b.set("weight", ev::fromUtf8(std::to_string(f.weight)));
    b.set("display", ev::fromUtf8("auto"));
    b.set("status", ev::fromUtf8(f.loaded ? "loaded" : "error"));
    ev::Persistent face(b.get());
    ev::Persistent loaded(resolvedPromise(face.get()));
    ev::setProperty(face.get(), "loaded", loaded.get());
    return face.get();
}

}  // namespace

Value makeFontFaceSet(std::function<dom::Document*()> docOf) {
    auto faces = [docOf]() {
        std::vector<FaceInfo> list = facesOf(docOf());
        return hostArrayOf(list.size(), [&list](size_t i) { return makeFontFace(list[i]); });
    };
    ObjectBuilder b;
    b.set("status", ev::fromUtf8("loaded"));
    b.accessor("size",
               [docOf](Value, std::span<const Value>) {
                   return ev::fromDouble(static_cast<double>(facesOf(docOf()).size()));
               },
               nullptr);
    b.def("check", 1, [](Value, std::span<const Value>) { return ev::fromBool(true); });
    b.def("load", 1, [faces](Value, std::span<const Value>) {
        ev::Persistent list(faces());
        return resolvedPromise(list.get());
    });
    b.def("has", 1, [](Value, std::span<const Value>) { return ev::fromBool(false); });
    b.def("add", 1, [](Value self, std::span<const Value>) { return self; });
    b.def("delete", 1, [](Value, std::span<const Value>) { return ev::fromBool(false); });
    b.def("clear", 0, [](Value, std::span<const Value>) { return ev::undefined(); });
    // Nothing loads later, so no loading / loadingdone / loadingerror event
    // ever fires.
    b.def("addEventListener", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
    b.def("removeEventListener", 2, [](Value, std::span<const Value>) { return ev::undefined(); });
    installSnapshotIteration(b.get(), faces);
    ev::Persistent set(b.get());
    ev::Persistent ready(resolvedPromise(set.get()));
    ev::setProperty(set.get(), "ready", ready.get());
    return set.get();
}

}  // namespace bro::bronze_host
