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
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_natives.h"
#include "bronze_host/host_window_open.h"
#include "engine/engine.h"
#include "util/log.h"

#if BRO_WITH_AUDIO
#include <broaudio/api.h>
#endif
#if BRO_WITH_GAMEAI
#include <brogameagent/api.h>
#endif
#if BRO_WITH_TENSOR
#include <brotensor/api.h>
#endif
#if BRO_WITH_LM
#include <brolm/api.h>
#endif
#if BRO_WITH_SOUNDML
#include <brosoundml/api.h>
#endif
#if BRO_WITH_DIFFUSION
#include <brodiffusion/api.h>
#endif
#if BRO_WITH_VISION
#include <brovisionml/api.h>
#endif
#include <broimage/api.h>
#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#if BRO_WITH_3D
#include "bronze_host/native_scene_internal.h"
#endif
#if BRO_WITH_GAMEAI
#include "engine/navmesh_subsystem.h"
#endif

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
    auto* bro = new ev::Persistent(makeRoot({"time", "window", "settings", "mesh", "net", "rigging", "gizmo",
                                             "scene", "terrain", "clipmap", "tile_world", "lighting", "animation", "lm",
                                             "rave", "motion", "mic", "sense", "gesture", "wake", "kws", "listen",
                                             "triposplat", "diffusion", "vision", "diar", "stt", "tts",
                                             "flora", "tensor", "impostor"}));
    auto* dunder = new ev::Persistent(
        makeRoot({"splash", "viewport", "perf", "bronze", "menu", "settingsUI", "inspector"}));
    auto* native = new ev::Persistent(
        makeRoot({"time", "window", "settings", "paths", "splash", "viewport", "perf", "bronze",
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

#if BRO_WITH_AUDIO
    broaudio::api::setAudioEngine(engine.audioEngine());
#endif
#if BRO_WITH_GAMEAI
    {
        brogameagent::api::NavMeshHooks hooks;
        hooks.registerNavMeshForPump = [](const std::shared_ptr<brogameagent::NavMesh>& m) {
            bro::engine::registerNavMeshForPump(m);
        };
        hooks.collectGeometry = [](bronze::Value rootVal, std::vector<float>& xyz, std::vector<uint32_t>& indices, std::string& err) -> bool {
            bronze::Value fromPhys = bronze::embed::getProperty(rootVal, "fromPhysics");
            if (!bronze::embed::isUndefined(fromPhys) && !bronze::embed::isNull(fromPhys) &&
                !(bronze::embed::isBool(fromPhys) && !bronze::embed::toBool(fromPhys))) {
#if BRO_WITH_PHYSICS
                physics::PhysicsWorld* world = unwrapPhysicsWorld(fromPhys);
                if (world) {
                    uint32_t layerMask = 0xffffffffu;
                    bronze::Value lv = bronze::embed::getProperty(rootVal, "physicsLayers");
                    if (bronze::embed::isObject(lv)) {
                        bronze::embed::Persistent lvRoot(lv);
                        bronze::Value lenV = bronze::embed::getProperty(lvRoot.get(), "length");
                        if (bronze::embed::isNumber(lenV)) {
                            uint32_t mask = 0;
                            uint32_t n = static_cast<uint32_t>(bronze::embed::toDouble(lenV));
                            for (uint32_t i = 0; i < n; i++) {
                                bronze::Value el = bronze::embed::getElement(lvRoot.get(), i);
                                int32_t idx = -1;
                                if (bronze::embed::isString(el)) {
                                    std::string s = bronze::embed::toUtf8(el);
                                    idx = world->layerIndex(s);
                                } else if (bronze::embed::isNumber(el)) {
                                    idx = static_cast<int32_t>(bronze::embed::toDouble(el));
                                }
                                if (idx >= 0 && idx < 32) mask |= (1u << idx);
                            }
                            layerMask = mask;
                        }
                    }
                    world->collectStaticTriangles(xyz, indices, layerMask);
                }
#endif
            }

            bronze::Value fromTerrainV = bronze::embed::getProperty(rootVal, "fromTerrain");
            if (!bronze::embed::isUndefined(fromTerrainV) && !bronze::embed::isNull(fromTerrainV)) {
#if BRO_WITH_3D
                HostTerrainCell* tc = bronze::embed::isObject(fromTerrainV) ? terrainCellOf(fromTerrainV) : nullptr;
                if (!tc) {
                    err = "bakeNavMesh: fromTerrain must be a scene.createTerrain() object";
                    return false;
                }
                bronze::Value boundsV = bronze::embed::getProperty(rootVal, "terrainBounds");
                if (!bronze::embed::isObject(boundsV)) {
                    err = "bakeNavMesh: fromTerrain requires terrainBounds: {minX, minZ, maxX, maxZ}";
                    return false;
                }
                bronze::embed::Persistent boundsRoot(boundsV);
                bronze::Value minXV = bronze::embed::getProperty(boundsRoot.get(), "minX");
                bronze::Value minZV = bronze::embed::getProperty(boundsRoot.get(), "minZ");
                bronze::Value maxXV = bronze::embed::getProperty(boundsRoot.get(), "maxX");
                bronze::Value maxZV = bronze::embed::getProperty(boundsRoot.get(), "maxZ");
                if (bronze::embed::isUndefined(minXV) || bronze::embed::isUndefined(minZV) ||
                    bronze::embed::isUndefined(maxXV) || bronze::embed::isUndefined(maxZV)) {
                    err = "bakeNavMesh: terrainBounds requires minX, minZ, maxX, maxZ";
                    return false;
                }
                float minX = static_cast<float>(bronze::embed::toDouble(minXV));
                float minZ = static_cast<float>(bronze::embed::toDouble(minZV));
                float maxX = static_cast<float>(bronze::embed::toDouble(maxXV));
                float maxZ = static_cast<float>(bronze::embed::toDouble(maxZV));
                float step = 1.0f;
                bronze::Value stepV = bronze::embed::getProperty(rootVal, "terrainStep");
                if (bronze::embed::isNumber(stepV)) step = static_cast<float>(bronze::embed::toDouble(stepV));
                if (step <= 0.0f) step = 1.0f;
                float rayStart = 100.0f;
                bronze::Value rsV = bronze::embed::getProperty(rootVal, "terrainRayStart");
                if (bronze::embed::isNumber(rsV)) rayStart = static_cast<float>(bronze::embed::toDouble(rsV));
                float rayLength = 200.0f;
                bronze::Value rlV = bronze::embed::getProperty(rootVal, "terrainRayLength");
                if (bronze::embed::isNumber(rlV)) rayLength = static_cast<float>(bronze::embed::toDouble(rlV));

                int nx = static_cast<int>(std::floor((maxX - minX) / step)) + 1;
                int nz = static_cast<int>(std::floor((maxZ - minZ) / step)) + 1;
                if (nx >= 2 && nz >= 2) {
                    uint32_t baseIdx = static_cast<uint32_t>(xyz.size() / 3);
                    for (int ix = 0; ix < nx; ++ix) {
                        float x = minX + ix * step;
                        for (int iz = 0; iz < nz; ++iz) {
                            float z = minZ + iz * step;
                            float y = 0.0f;
                            terrainSampleHeight(tc, x, z, rayStart, rayLength, y);
                            xyz.push_back(x);
                            xyz.push_back(y);
                            xyz.push_back(z);
                        }
                    }
                    for (int ix = 0; ix < nx - 1; ++ix) {
                        for (int iz = 0; iz < nz - 1; ++iz) {
                            uint32_t v00 = baseIdx + ix * nz + iz;
                            uint32_t v01 = baseIdx + ix * nz + (iz + 1);
                            uint32_t v10 = baseIdx + (ix + 1) * nz + iz;
                            uint32_t v11 = baseIdx + (ix + 1) * nz + (iz + 1);
                            indices.push_back(v00);
                            indices.push_back(v01);
                            indices.push_back(v10);
                            indices.push_back(v10);
                            indices.push_back(v01);
                            indices.push_back(v11);
                        }
                    }
                }
#else
                err = "bakeNavMesh: fromTerrain requires a 3D-enabled build";
                return false;
#endif
            }
            return true;
        };
        hooks.collectObstacles = [](bronze::Value rootVal, float minX, float maxX, float minZ, float maxZ,
                                    std::vector<brogameagent::AABB>& outBoxes, std::string&) -> bool {
#if BRO_WITH_PHYSICS
            bronze::Value fromPhys = bronze::embed::getProperty(rootVal, "fromPhysics");
            physics::PhysicsWorld* world = unwrapPhysicsWorld(fromPhys);
            if (world) {
                uint32_t layerMask = 0xffffffffu;
                bronze::Value lv = bronze::embed::getProperty(rootVal, "physicsLayers");
                if (bronze::embed::isObject(lv)) {
                    bronze::embed::Persistent lvRoot(lv);
                    bronze::Value lenV = bronze::embed::getProperty(lvRoot.get(), "length");
                    if (bronze::embed::isNumber(lenV)) {
                        uint32_t mask = 0;
                        uint32_t n = static_cast<uint32_t>(bronze::embed::toDouble(lenV));
                        for (uint32_t i = 0; i < n; i++) {
                            bronze::Value el = bronze::embed::getElement(lvRoot.get(), i);
                            int32_t idx = -1;
                            if (bronze::embed::isString(el)) {
                                std::string s = bronze::embed::toUtf8(el);
                                idx = world->layerIndex(s);
                            } else if (bronze::embed::isNumber(el)) {
                                idx = static_cast<int32_t>(bronze::embed::toDouble(el));
                            }
                            if (idx >= 0 && idx < 32) mask |= (1u << idx);
                        }
                        layerMask = mask;
                    }
                }
                float bandMinY = -1e9f;
                float bandMaxY = 1e9f;
                bronze::Value minyV = bronze::embed::getProperty(rootVal, "physicsMinY");
                if (bronze::embed::isNumber(minyV)) bandMinY = static_cast<float>(bronze::embed::toDouble(minyV));
                bronze::Value maxyV = bronze::embed::getProperty(rootVal, "physicsMaxY");
                if (bronze::embed::isNumber(maxyV)) bandMaxY = static_cast<float>(bronze::embed::toDouble(maxyV));

                for (const auto& b : world->collectStaticBodies()) {
                    if (b.isSensor) continue;
                    if (b.layer >= 0 && b.layer < 32 && !(layerMask & (1u << b.layer))) continue;
                    if (b.max.GetY() < bandMinY || b.min.GetY() > bandMaxY) continue;
                    if (b.min.GetX() <= minX && b.max.GetX() >= maxX &&
                        b.min.GetZ() <= minZ && b.max.GetZ() >= maxZ) continue;
                    brogameagent::AABB box{
                        0.5f * (b.min.GetX() + b.max.GetX()),
                        0.5f * (b.min.GetZ() + b.max.GetZ()),
                        0.5f * (b.max.GetX() - b.min.GetX()),
                        0.5f * (b.max.GetZ() - b.min.GetZ()),
                    };
                    outBoxes.push_back(box);
                }
            }
#endif
            return true;
        };
        brogameagent::api::setNavMeshHooks(hooks);
        brogameagent::api::installGameAi();
    }
#endif
#if BRO_WITH_TENSOR
    brotensor::api::installTensor();
#endif
#if BRO_WITH_LM
    brolm::api::installLM();
#endif
#if BRO_WITH_SOUNDML
    brosoundml::api::installSoundML();
#endif
#if BRO_WITH_DIFFUSION
    brodiffusion::api::installDiffusion();
#endif
#if BRO_WITH_VISION
    brovisionml::api::installVision();
#endif
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
    setUnavailable("gizmo", "BRO_WITH_3D");
    setUnavailable("impostor", "BRO_WITH_3D");
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
        // `bro.image` is ONE object with three sources: brokit's kernels
        // (which create it, now that `bro` is registered), the codecs
        // and ops from broimage_api, and `gpu`, which
        // installImageGpuModule mounts right after this returns.
        installBrokitImageKernels();
        broimage::api::installImage();
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
    installSettingsObserver(engine);
    installBroCoreModule();
}

}  // namespace bro::bronze_host
