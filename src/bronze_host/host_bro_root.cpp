// The `bro` and `__bro` roots, the `__bro_native` root under them, and the
// registration helpers every native_*.cpp uses. host_natives.h states the
// convention this file is the anchor of.
//
// THREE PLAIN OBJECTS are registered as host globals here. `bro` carries the
// public namespaces the wrapper fills (time, window, settings) and the bare
// path members (appDir, userDataDir, resolvePath); `__bro` the system
// panels' namespaces; `__bro_native` one empty object per native namespace,
// so a DYNAMIC read of `__bro_native.perf` — the one the wrapper makes to
// ask whether the 3D counters exist — lands on an object rather than
// undefined. The natives themselves are not properties of anything: a
// compiled `__bro_native.time.scale` is a direct call, and the object under
// it is never consulted.
//
// ORDER: roots first (a module's bare reads of `bro` / `__bro` /
// `__bro_native` must find the registry full when its entry runs), then the
// sibling libraries' APIs, once each (host_sibling_apis.cpp), then the
// natives (registration is per thread and needs no engine), then the engine
// hook the settings callback rides on, then js/bro_core.js, which fills the
// public objects from the natives.

#include "bronze_host/host_internal.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_natives.h"
#include "bronze_host/host_window_open.h"
#include "engine/engine.h"
#include "util/log.h"

#include <string>
#include <vector>

namespace bro::bronze_host {

namespace natives {

namespace {

ev::NativeSignature sig(const char* ret, std::initializer_list<const char*> params,
                        ev::NativeKind kind) {
    ev::NativeSignature s;
    s.returnType = ret;
    for (const char* p : params) s.paramTypes.emplace_back(p);
    s.kind = kind;
    return s;
}

}  // namespace

bool fn(const char* path, void* f, const char* ret,
        std::initializer_list<const char*> params, std::string* error) {
    return ev::registerNative(path, f, sig(ret, params, ev::NativeKind::Function), error);
}

bool getter(const char* path, void* f, const char* ret, std::string* error) {
    return ev::registerNative(path, f, sig(ret, {}, ev::NativeKind::Getter), error);
}

bool setter(const char* path, void* f, const char* type, std::string* error) {
    return ev::registerNative(path, f, sig("void", {type}, ev::NativeKind::Setter), error);
}

bool ctor(const char* path, void* f, void (*dtor)(void*),
          std::initializer_list<const char*> params, std::string* error) {
    ev::NativeSignature s = sig(path, params, ev::NativeKind::Constructor);
    s.className = path;
    s.destructor = dtor;
    return ev::registerNative(path, f, s, error);
}

const char* strResult(std::string s) {
    thread_local std::string scratch;
    scratch = std::move(s);
    return scratch.c_str();
}

}  // namespace natives

bool registerBroNatives(std::string* error) {
    bool ok = registerTimeNatives(error) &&
              registerServerNatives(error) &&
              registerPathsNatives(error) &&
              registerWindowNatives(error) &&
              registerSettingsNatives(error) &&
              registerDunderBroNatives(error) &&
              registerMeshNatives(error) &&
              registerNetNatives(error) &&
              registerRiggingNatives(error) &&
              registerPhysicsNatives(error) &&
              registerLmNatives(error) &&
              registerRaveNatives(error) &&
              registerMotionNatives(error) &&
              registerMicNatives(error) &&
              registerSenseNatives(error) &&
              registerGestureNatives(error) &&
              registerWakeNatives(error) &&
              registerKwsNatives(error) &&
              registerListenNatives(error) &&
              registerTriposplatNatives(error) &&
              registerDiffusionNatives(error) &&
              registerVisionNatives(error) &&
              registerDiarNatives(error) &&
              registerSttNatives(error) &&
              registerTtsNatives(error) &&
              registerFloraNatives(error) &&
              registerTensorNatives(error);
#if BRO_WITH_3D
    ok = ok &&
         registerAnimationNatives(error) &&
         registerTerrainNatives(error) &&
         registerSceneNatives(error) &&
         registerClipmapNatives(error) &&
         registerTileWorldNatives(error) &&
         registerGizmoNatives(error) &&
         registerLightingNatives(error);
#endif
    return ok;
}

// bronze_host.h: the calling thread's registry as the manifest file an
// ahead-of-time compile takes.
bool writeNativeManifest(const std::string& path, std::string* error) {
    return ev::writeNativeManifest(path, error);
}

namespace {

// A root object with one empty plain object per name. Every allocation is
// rooted before the next one: setProperty may move the root and answers its
// current address.
ev::Persistent makeRoot(const std::vector<const char*>& namespaces) {
    ev::Persistent root(ev::createObject());
    for (const char* name : namespaces) {
        ev::Persistent ns(ev::createObject());
        root.set(ev::setProperty(root.get(), name, ns.get()));
    }
    return root;
}

void publish(const char* name, const ev::Persistent& root) {
    ev::registerGlobal(name, root.get());
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) ev::setProperty(gt.value, name, root.get());
}

}  // namespace

Value makeUnavailableNamespace(const std::string& name, const std::string& flag) {
    ObjectBuilder base;
    base.set("available", ev::fromBool(false));

    HostProxyTraps traps;
    traps.methods = base.get();
    traps.get = [name, flag](const std::string& key, Value& out) -> bool {
        std::string err = "bro." + name + " is unavailable: this build was compiled without " + flag;
        out = ev::makeFunction([err](Value, std::span<const Value>) -> Value {
            return ev::throwError(err.c_str());
        }, 0);
        return true;
    };
    traps.has = [](const std::string& key) -> bool {
        return key == "available";
    };
    traps.ownKeys = []() -> std::vector<std::string> {
        return { "available" };
    };
    return makeHostProxy(std::move(traps));
}

void installBroRoots(engine::Engine& engine) {
    // Heap-allocated and never freed, like every root this layer keeps for
    // the life of the process (host_internal.h, HostClass).
    auto* bro = new ev::Persistent(makeRoot({"time", "server", "window", "settings", "mesh", "net", "rigging", "gizmo",
                                             "scene", "terrain", "clipmap", "tile_world", "lighting", "animation", "lm",
                                             "rave", "motion", "mic", "sense", "gesture", "wake", "kws", "listen",
                                             "triposplat", "diffusion", "vision", "diar", "stt", "tts",
                                             "flora", "tensor", "impostor"}));
    auto* dunder = new ev::Persistent(
        makeRoot({"splash", "viewport", "perf", "bronze", "menu", "settingsUI", "inspector"}));
    auto* native = new ev::Persistent(
        makeRoot({"time", "server", "window", "settings", "paths", "splash", "viewport", "perf", "bronze",
                   "menu", "settingsUI", "inspector", "mesh", "net", "rigging", "physics",
                   "animation", "terrain", "clipmap", "tile_world", "lighting", "gizmo", "scene", "lm",
                   "rave", "motion", "mic", "sense", "gesture", "wake", "kws", "listen",
                   "triposplat", "diffusion", "vision", "diar", "stt", "tts",
                   "flora", "tensor"}));
    auto* physicsRoot = new ev::Persistent(ev::createObject());
#if BRO_WITH_3D
    {
        ev::Persistent scene(ev::createObject());
        Value perf = ev::getProperty(native->get(), "perf");
        ev::setProperty(perf, "scene", scene.get());
    }
#endif
    publish("bro", *bro);
    publish("__bro", *dunder);
    publish("__bro_native", *native);
    publish("Physics", *physicsRoot);

    // Every sibling's JS API, once (host_sibling_apis.cpp): they mount onto
    // the roots just published and register the natives bro_core.js and the
    // manifest below are built over.
    installSiblingApis(engine);
    {
        ev::Persistent math(makeBroMathValue());
        ev::setProperty(bro->get(), "math", math.get());
    }
    {
        ev::Persistent text(makeBroTextValue());
        ev::setProperty(bro->get(), "text", text.get());
    }
    {
        ev::Persistent gpu(makeBroGpuValue());
        ev::setProperty(bro->get(), "gpu", gpu.get());
    }
    {
        ev::Persistent steam(makeBroSteamValue());
        ev::setProperty(bro->get(), "steam", steam.get());
    }
    {
        ev::Persistent menu(makeBroMenuValue());
        ev::setProperty(bro->get(), "menu", menu.get());
    }
    {
        ev::Persistent media(makeBroMediaValue());
        ev::setProperty(bro->get(), "media", media.get());
    }

    // Feature-gated unavailable namespace stubs (Proxy throwing on any call)
    auto setUnavailable = [&](const char* name, const char* flag) {
        ev::Persistent stub(makeUnavailableNamespace(name, flag));
        ev::setProperty(bro->get(), name, stub.get());
    };

#if !BRO_WITH_VIDEO
    setUnavailable("media", "BRO_WITH_VIDEO");
#endif
#if !BRO_WITH_NET
    setUnavailable("net", "BRO_WITH_NET");
#endif
#if !BRO_WITH_FLORA
    setUnavailable("flora", "BRO_WITH_FLORA");
#endif
#if !BRO_WITH_GAMEAI
    setUnavailable("ai", "BRO_WITH_GAMEAI");
#endif
#if !BRO_WITH_3D
    // Everything the 3D families own: bromesh's namespaces (bromesh_api is
    // not linked, host_sibling_apis.cpp) and the scene-graph natives (not in
    // the library, CMakeLists.txt). Without this the makeRoot placeholders
    // would read as empty objects rather than as the documented stub.
    setUnavailable("mesh", "BRO_WITH_3D");
    setUnavailable("rigging", "BRO_WITH_3D");
    setUnavailable("scene", "BRO_WITH_3D");
    setUnavailable("terrain", "BRO_WITH_3D");
    setUnavailable("clipmap", "BRO_WITH_3D");
    setUnavailable("tile_world", "BRO_WITH_3D");
    setUnavailable("lighting", "BRO_WITH_3D");
    setUnavailable("animation", "BRO_WITH_3D");
    setUnavailable("gizmo", "BRO_WITH_3D");
    setUnavailable("impostor", "BRO_WITH_3D");
#endif
#if !BRO_WITH_PHYSICS
    // The `Physics` global js/physics.js would have filled: the same stub
    // shape, so Physics.available === false and any call names the flag.
    {
        ev::Persistent stub(makeUnavailableNamespace("physics", "BRO_WITH_PHYSICS"));
        publish("Physics", stub);
    }
#endif
#if !BRO_WITH_AUDIO
    // bro.mic is broaudio's (installAudio mounts it on the makeRoot
    // placeholder); without the stub the empty placeholder would be marked
    // available by markAvailableNamespaces and `bro.mic.start` would be
    // undefined rather than a call that names the flag.
    setUnavailable("mic", "BRO_WITH_AUDIO");
#endif
#if !BRO_WITH_LM
    setUnavailable("lm", "BRO_WITH_LM");
#endif
#if !BRO_WITH_SOUNDML
    setUnavailable("stt", "BRO_WITH_SOUNDML");
    setUnavailable("tts", "BRO_WITH_SOUNDML");
    setUnavailable("diar", "BRO_WITH_SOUNDML");
    setUnavailable("rave", "BRO_WITH_SOUNDML");
    setUnavailable("wake", "BRO_WITH_SOUNDML");
    setUnavailable("kws", "BRO_WITH_SOUNDML");
    setUnavailable("sense", "BRO_WITH_SOUNDML");
    setUnavailable("gesture", "BRO_WITH_SOUNDML");
    setUnavailable("listen", "BRO_WITH_SOUNDML");
#endif
#if !BRO_WITH_VISION
    setUnavailable("vision", "BRO_WITH_VISION");
#endif
#if !BRO_WITH_DIFFUSION
    setUnavailable("diffusion", "BRO_WITH_DIFFUSION");
#endif
#if !BRO_WITH_TENSOR
    setUnavailable("tensor", "BRO_WITH_TENSOR");
#endif
#if !BRO_WITH_TRIPOSPLAT
    setUnavailable("triposplat", "BRO_WITH_TRIPOSPLAT");
#endif
#if !(BRO_WITH_DIFFUSION && BRO_WITH_LM)
    setUnavailable("motion", "BRO_WITH_DIFFUSION+BRO_WITH_LM");
#endif
    {
        // installBroWindowOpen roots it and installs `parent` as well.
        Value broWin = ev::getProperty(bro->get(), "window");
        if (ev::isObject(broWin)) installBroWindowOpen(broWin);
    }

    std::string err;
    if (!registerBroNatives(&err)) {
        LOG_ERROR("bronze_host: native registration failed: %s", err.c_str());
    }
    installSettingsObserver(engine);
    installBroCoreModule();
}

// `bro.<ns>.available` on every feature-gated namespace: the compiled-out
// stub (makeUnavailableNamespace) answers false, so a compiled-in namespace
// must answer TRUE rather than undefined — `if (bro.tts.available)` is the
// documented probe (docs/*-api.js) and a missing flag reads as "no". Runs
// after every wrapper module has mounted, so it decorates the final objects;
// a namespace that already carries `available` (a sibling that reports its
// own runtime probe, e.g. bro.net's transport) is left alone.
void markAvailableNamespaces() {
    static const char* const kGated[] = {
        "tts", "lm", "stt", "diar", "net", "ai", "gesture", "gizmo", "impostor", "kws",
        "listen", "motion", "rave", "sense", "triposplat", "vision", "wake", "diffusion",
        "tensor", "flora", "media", "mesh", "rigging", "scene", "terrain", "clipmap",
        "tile_world", "lighting", "animation", "mic",
    };
    auto decorate = [](Value nsIn) {
        if (!ev::isObject(nsIn)) return;
        // Rooted across the read, which may run a getter and allocate.
        ev::Persistent ns(nsIn);
        Value cur = ev::getProperty(ns.get(), "available");
        if (!ev::isUndefined(cur)) return;
        ev::setProperty(ns.get(), "available", ev::fromBool(true));
    };
    ev::GlobalValue broG = ev::globalValue("bro");
    if (broG.found && ev::isObject(broG.value)) {
        for (const char* name : kGated) {
            ev::Persistent root(broG.value);
            decorate(ev::getProperty(root.get(), name));
        }
    }
    ev::GlobalValue phys = ev::globalValue("Physics");
    if (phys.found) decorate(phys.value);
}

// The worker realm's roots, the same shape as the main realm's cut down to
// what a worker owns: `bro` and `__bro_native` with the namespaces below,
// the natives a worker carries registered on THIS thread (bronze's registry
// is per thread, and a wrapper module binds its import table against the
// thread that enters it), the sibling libraries' compute APIs over them
// (installWorkerSiblingApis), then the wrapper modules — js/net_sync.js,
// js/net.js, js/motion.js, js/server.js — in the main realm's order.
// bro_core.js is not entered: it is compiled against every native and would
// refuse to bind on a thread that registered only these. A feature this
// build compiled out gets the same `available: false` stub the main realm
// shows. The engine-bound namespaces (time, window, settings, the scene,
// audio) are not here at all.
void installWorkerBroRoot() {
    auto* bro = new ev::Persistent(makeRoot({"net", "server", "motion"}));
    auto* native = new ev::Persistent(makeRoot({"net", "server", "motion"}));
    publish("bro", *bro);
    publish("__bro_native", *native);

    std::string err;
    bool ok = registerServerNatives(&err) && registerMotionNatives(&err);
#if BRO_WITH_NET
    ok = ok && registerNetNatives(&err);
#endif
    if (!ok) {
        LOG_ERROR("bronze_host: worker native registration failed: %s", err.c_str());
        return;
    }

    installWorkerSiblingApis();
    {
        ev::Persistent math(makeBroMathValue());
        ev::setProperty(bro->get(), "math", math.get());
    }
    {
        ev::Persistent gpu(makeBroGpuValue());
        ev::setProperty(bro->get(), "gpu", gpu.get());
    }
    {
        ev::Persistent media(makeBroMediaValue());
        ev::setProperty(bro->get(), "media", media.get());
    }

    auto setUnavailable = [&](const char* name, const char* flag) {
        ev::Persistent stub(makeUnavailableNamespace(name, flag));
        ev::setProperty(bro->get(), name, stub.get());
    };
#if BRO_WITH_NET
    installNetSyncModule();
    installNetModule();
#else
    setUnavailable("net", "BRO_WITH_NET");
#endif
#if BRO_WITH_DIFFUSION && BRO_WITH_LM
    installMotionModule();
#else
    setUnavailable("motion", "BRO_WITH_DIFFUSION+BRO_WITH_LM");
#endif
    installServerModule();
#if !BRO_WITH_VIDEO
    setUnavailable("media", "BRO_WITH_VIDEO");
#endif
#if !BRO_WITH_FLORA
    setUnavailable("flora", "BRO_WITH_FLORA");
#endif
#if !BRO_WITH_GAMEAI
    setUnavailable("ai", "BRO_WITH_GAMEAI");
#endif
#if !BRO_WITH_3D
    setUnavailable("mesh", "BRO_WITH_3D");
    setUnavailable("rigging", "BRO_WITH_3D");
#endif
#if !BRO_WITH_LM
    setUnavailable("lm", "BRO_WITH_LM");
#endif
#if !BRO_WITH_SOUNDML
    setUnavailable("stt", "BRO_WITH_SOUNDML");
    setUnavailable("tts", "BRO_WITH_SOUNDML");
    setUnavailable("diar", "BRO_WITH_SOUNDML");
    setUnavailable("rave", "BRO_WITH_SOUNDML");
#endif
#if !BRO_WITH_VISION
    setUnavailable("vision", "BRO_WITH_VISION");
#endif
#if !BRO_WITH_DIFFUSION
    setUnavailable("diffusion", "BRO_WITH_DIFFUSION");
#endif
#if !BRO_WITH_TENSOR
    setUnavailable("tensor", "BRO_WITH_TENSOR");
#endif
#if !BRO_WITH_TRIPOSPLAT
    setUnavailable("triposplat", "BRO_WITH_TRIPOSPLAT");
#endif
    markAvailableNamespaces();
}

}  // namespace bro::bronze_host
