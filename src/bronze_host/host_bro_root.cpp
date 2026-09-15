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
// natives (registration is per thread and needs no engine), then the engine
// hook the settings callback rides on, then js/bro_core.js, which fills the
// public objects from the natives.

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "bronze_host/host_window_open.h"
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
              registerVisionNatives(error);
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

void installBroRoots(engine::Engine& engine) {
    // Heap-allocated and never freed, like every root this layer keeps for
    // the life of the process (host_internal.h, HostClass).
    auto* bro = new ev::Persistent(makeRoot({"time", "window", "settings", "mesh", "net", "rigging", "gizmo",
                                             "scene", "terrain", "clipmap", "tile_world", "lighting", "animation", "lm",
                                             "rave", "motion", "mic", "sense", "gesture", "wake", "kws", "listen",
                                             "triposplat", "diffusion", "vision"}));
    auto* dunder = new ev::Persistent(
        makeRoot({"splash", "viewport", "perf", "bronze", "menu", "settingsUI", "inspector"}));
    auto* native = new ev::Persistent(
        makeRoot({"time", "window", "settings", "paths", "splash", "viewport", "perf", "bronze",
                  "menu", "settingsUI", "inspector", "mesh", "net", "rigging", "physics",
                  "animation", "terrain", "clipmap", "tile_world", "lighting", "gizmo", "scene", "lm",
                  "rave", "motion", "mic", "sense", "gesture", "wake", "kws", "listen",
                  "triposplat", "diffusion", "vision"}));
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

    {
        ev::Persistent ai(makeBroAiValue());
        ev::setProperty(bro->get(), "ai", ai.get());
    }
    {
        ev::Persistent math(makeBroMathValue());
        ev::setProperty(bro->get(), "math", math.get());
    }
    {
        ev::Persistent text(makeBroTextValue());
        ev::setProperty(bro->get(), "text", text.get());
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
    {
        Value broImg = ev::getProperty(bro->get(), "image");
        if (!ev::isObject(broImg)) {
            ev::Persistent img(ev::createObject());
            ev::setProperty(bro->get(), "image", img.get());
            broImg = img.get();
        }
        Value codecImg = makeBroImageValue();
        for (const char* prop : {"transcodeKTX2", "encodePngFile", "encodePng", "encodeJpegFile", "encodeJpeg"}) {
            Value fn = ev::getProperty(codecImg, prop);
            if (!ev::isUndefined(fn)) ev::setProperty(broImg, prop, fn);
        }
        Value gpu = ev::globalValue("__bro_image_gpu").value;
        if (ev::isObject(gpu)) {
            ev::setProperty(broImg, "gpu", gpu);
        }
    }
    {
        Value broWin = ev::getProperty(bro->get(), "window");
        if (ev::isObject(broWin)) {
            installBroWindowOpen(broWin);
            installBroWindowParent(broWin);
        }
    }

    std::string err;
    if (!registerBroNatives(&err)) {
        LOG_ERROR("bronze_host: native registration failed: %s", err.c_str());
    }
    // The mesh classes' prototypes onto `__bro_native.mesh`, now that the
    // constructors are registered (js/mesh.js chains them; host_natives.h).
    publishMeshPrototypes(native->get());
    installSettingsObserver(engine);
    installBroCoreModule();
}

}  // namespace bro::bronze_host
