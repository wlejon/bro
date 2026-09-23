// The sibling libraries' JavaScript APIs, installed ONCE per realm.
//
// Each bro-* sibling ships an api library (../<name>/src/api) whose
// install*() mounts its namespace onto the `bro` root, registers natives
// under `__bro_native`, and/or enters its own bronze-compiled JS. None of
// them is re-entrant: brotensor's registerNative fatals on a second
// registration of the same path, and the HostClass-based ones rebuild every
// class on each call (a new constructor, a new prototype, Persistents that
// are never freed), so a second call hands out a second identity for `Mesh`
// or `LMModel` and breaks instanceof against the first. The rule is
// therefore ONE call site per sibling per realm, and this file is it:
// nothing else in bro may call a sibling's install*().
//
// ORDER. installBroRoots calls this after `bro`, `__bro` and `__bro_native`
// are published and before registerBroNatives / js/bro_core.js. Every
// installer below wants at least one of those roots to exist already:
//   - brotensor::installTensor  reads `__bro_native` (creates one if absent,
//     which would shadow the real root) and its tensor.js reads `bro`;
//   - brolm / brosoundml / brodiffusion / brovisionml / brogameagent /
//     bromesh / broimage look up `bro` and, absent one, register their OWN
//     — a second `bro` object the rest of the roots would then miss;
//   - broaudio::installMic reads `bro` and re-sets the global to what it
//     found, so it too must see the real root;
//   - broflora's flora.js reads `__bro_native.flora`.
// The bro-side compiled modules (host_js_modules.cpp) list none of these
// names in js/module.globals; they reach `Mesh`, `bro.mesh` and the rest
// through globalThis at the point of use, so they carry no load-time
// dependency on this file beyond the roots themselves.
//
// The adoptGlobalProperty lifts turn a class a sibling only ASSIGNED onto
// globalThis (brolm, broflora) into a host global a compiled app can read
// by its bare name. bromesh registers its own; the tensor lift is kept for
// symmetry and is a no-op while tensor.js leaves GpuTensor under bro.tensor.

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"

#if BRO_WITH_AUDIO
#include <broaudio/api.h>
#endif
#if BRO_WITH_GAMEAI
#include <brogameagent/api.h>
#include "engine/navmesh_subsystem.h"
#endif
#if BRO_WITH_TENSOR
#include <brotensor/api.h>
#endif
#if BRO_WITH_LM
#include <brolm/api.h>
#endif
#include "api/api.h"  // brokit::api::resolveAssetPath

#if BRO_WITH_SOUNDML
#include <brosoundml/api.h>
#include "audio_inference/audio_inference.h"
#include "util/log.h"
#endif
#if BRO_WITH_DIFFUSION
#include <brodiffusion/api.h>
#endif
#if BRO_WITH_VISION
#include <brovisionml/api.h>
#include "bronze_host/host_vision.h"
#endif
#if BRO_WITH_FLORA
#include <broflora/api/api.h>
#endif
#if BRO_WITH_3D
#include <bromesh/api.h>
#include "bronze_host/native_scene_internal.h"
#endif
#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#include "api/api.h"  // brokit::api::resolveAssetPath
#include <broimage/api.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

#if BRO_WITH_GAMEAI

#if BRO_WITH_PHYSICS
// The `physicsLayers` option of bakeNavMesh: names or indices into a bit
// mask, everything when absent.
uint32_t physicsLayerMask(bronze::Value rootVal, physics::PhysicsWorld* world) {
    uint32_t layerMask = 0xffffffffu;
    bronze::Value lv = bronze::embed::getProperty(rootVal, "physicsLayers");
    if (!bronze::embed::isObject(lv)) return layerMask;
    bronze::embed::Persistent lvRoot(lv);
    bronze::Value lenV = bronze::embed::getProperty(lvRoot.get(), "length");
    if (!bronze::embed::isNumber(lenV)) return layerMask;
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
    return mask;
}
#endif

bool collectNavGeometry(bronze::Value rootVal, std::vector<float>& xyz,
                        std::vector<uint32_t>& indices, std::string& err) {
    bronze::Value fromPhys = bronze::embed::getProperty(rootVal, "fromPhysics");
    if (!bronze::embed::isUndefined(fromPhys) && !bronze::embed::isNull(fromPhys) &&
        !(bronze::embed::isBool(fromPhys) && !bronze::embed::toBool(fromPhys))) {
#if BRO_WITH_PHYSICS
        physics::PhysicsWorld* world = unwrapPhysicsWorld(fromPhys);
        if (world) world->collectStaticTriangles(xyz, indices, physicsLayerMask(rootVal, world));
#endif
    }

    bronze::Value fromTerrainV = bronze::embed::getProperty(rootVal, "fromTerrain");
    if (bronze::embed::isUndefined(fromTerrainV) || bronze::embed::isNull(fromTerrainV)) return true;
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
    return true;
#else
    err = "bakeNavMesh: fromTerrain requires a 3D-enabled build";
    return false;
#endif
}

bool collectNavObstacles(bronze::Value rootVal, float minX, float maxX, float minZ, float maxZ,
                         std::vector<brogameagent::AABB>& outBoxes, std::string&) {
#if BRO_WITH_PHYSICS
    bronze::Value fromPhys = bronze::embed::getProperty(rootVal, "fromPhysics");
    physics::PhysicsWorld* world = unwrapPhysicsWorld(fromPhys);
    if (!world) return true;
    uint32_t layerMask = physicsLayerMask(rootVal, world);
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
#else
    (void)rootVal; (void)minX; (void)maxX; (void)minZ; (void)maxZ; (void)outBoxes;
#endif
    return true;
}

void installGameAiApi() {
    brogameagent::api::NavMeshHooks hooks;
    hooks.registerNavMeshForPump = [](const std::shared_ptr<brogameagent::NavMesh>& m) {
        bro::engine::registerNavMeshForPump(m);
    };
    hooks.collectGeometry = &collectNavGeometry;
    hooks.collectObstacles = &collectNavObstacles;
    brogameagent::api::setNavMeshHooks(hooks);
    brogameagent::api::installGameAi();
}

#endif  // BRO_WITH_GAMEAI

}  // namespace

void installSiblingApis(engine::Engine& engine) {
#if BRO_WITH_AUDIO
    // AudioContext and the node classes need no root; bro.mic mounts onto
    // the `bro` root and re-sets the global to the object it found.
    broaudio::api::setAudioEngine(engine.audioEngine());
    // Its file loaders, savers and preset IO take paths the way fs.* does.
    broaudio::api::setPathResolver(&brokit::api::resolveAssetPath);
    broaudio::api::installAudio();
    broaudio::api::installMic();
#else
    (void)engine;
#endif
#if BRO_WITH_GAMEAI
    installGameAiApi();
#endif
#if BRO_WITH_3D
    // bro.mesh / bro.rigging and their classes (Mesh, MeshBVH, Skeleton,
    // ...), which bromesh registers as host globals itself. Its file
    // loaders and savers take paths the way fs.* does.
    bromesh::api::setPathResolver(&brokit::api::resolveAssetPath);
    bromesh::api::installMesh();
    bromesh::api::installRigging();
#endif
#if BRO_WITH_TENSOR
    brotensor::api::setPathResolver(&brokit::api::resolveAssetPath);
    brotensor::api::installTensor();
    adoptGlobalProperty("GpuTensor");
#endif
#if BRO_WITH_LM
    brolm::api::setPathResolver(&brokit::api::resolveAssetPath);
    brolm::api::installLM();
    for (const char* name : {"AsyncHandle", "QwenTokenizer", "MistralTokenizer", "GemmaTokenizer",
                             "Llama3Tokenizer", "LMModel", "Qwen35Model", "Qwen3VLModel", "NllbModel",
                             "ClipModel", "T5Model", "LayaModel", "ModernBertModel", "Grammar"}) {
        adoptGlobalProperty(name);
    }
    {
        // The LM tick fires generate() callbacks and settles LayaModel
        // promises (predictAsync / loadLayaAsync). Their reactions run right
        // after, in this frame, before it renders: a result the device
        // finished mid-frame reaches the page one frame sooner than waiting
        // for the next frame's microtask checkpoint. The shutdown hook stops
        // every Laya scheduler's device threads before the runtime and
        // brotensor go away.
        static bool lmHooksInstalled = false;
        if (!lmHooksInstalled) {
            lmHooksInstalled = true;
            engine.addFramePump([] {
                brolm::api::tickLMAsync();
                if (ev::microtasksPending()) ev::drainMicrotasks();
            });
            engine.addShutdownHook([] { brolm::api::shutdownLM(); });
        }
    }
#endif
#if BRO_WITH_SOUNDML
    // One installer covers stt, tts, diar, rave, wake, kws, sense, gesture
    // and listen. Its async jobs (background loads / inferences) deliver
    // their JS callbacks from the engine's frame pump, and are cancelled +
    // joined at shutdown before the runtime and brotensor go away. The
    // engine is one per process, so the hooks register once even though a
    // reload re-runs the installers for the new realm.
    //
    // The listen host (bro.listen and the wake / kws / sense / gesture
    // tenants) taps the engine's broaudio for its mic streams and runs each
    // stream's feed as a pump on the engine's AudioInference worker — off
    // the audio thread and off the main thread windowed, and inline under
    // the virtual clock headless (stepInline from advanceTime), which is
    // what keeps a scripted feed() deterministic. removeTask is the barrier
    // the host relies on to mutate a stream's models after a detach.
    brosoundml::api::setPathResolver(&brokit::api::resolveAssetPath);
    brosoundml::api::setLogHook([](const std::string& line) { LOG_INFO("%s", line.c_str()); });
    brosoundml::api::setAudioEngine(engine.audioEngine());
    {
        engine::AudioInference* inference = engine.audioInference();
        brosoundml::api::InferenceScheduler sched;
        if (inference) {
            sched.addPump = [inference](std::function<void()> pump) {
                return inference->addPump(std::move(pump));
            };
            sched.removePump = [inference](std::uint32_t id) { inference->removeTask(id); };
            sched.threaded = [inference] { return inference->threaded(); };
        }
        brosoundml::api::setInferenceScheduler(std::move(sched));
    }
    brosoundml::api::installSoundML();
    {
        static bool soundmlHooksInstalled = false;
        if (!soundmlHooksInstalled) {
            soundmlHooksInstalled = true;
            engine.addFramePump([] { brosoundml::api::tickSoundML(); });
            engine.addShutdownHook([] { brosoundml::api::shutdownSoundML(); });
        }
    }
#endif
#if BRO_WITH_DIFFUSION
    // bro.diffusion and bro.triposplat together.
    brodiffusion::api::setPathResolver(&brokit::api::resolveAssetPath);
    brodiffusion::api::installDiffusion();
    {
        // A background generate (generateAsync, or generate/imageToImage/
        // inpaint with onDone or async: true) delivers its onDone from this
        // tick; without it the result waits for a script that calls
        // bro.diffusion.tick() itself. Their reactions run in the same
        // frame, as the LM tick's do. Ungated by bro.time pause, like every
        // frame pump. The shutdown hook cancels and joins the jobs still
        // running before the runtime and brotensor go away.
        static bool diffusionHooksInstalled = false;
        if (!diffusionHooksInstalled) {
            diffusionHooksInstalled = true;
            engine.addFramePump([] {
                brodiffusion::api::tickDiffusionAsync();
                if (ev::microtasksPending()) ev::drainMicrotasks();
            });
            engine.addShutdownHook([] { brodiffusion::api::shutdownDiffusionAsync(); });
        }
    }
#endif
#if BRO_WITH_VISION
    brovisionml::api::setPathResolver(&brokit::api::resolveAssetPath);
    brovisionml::api::installVision();
    // ...then bro's half: the `image` / `matte` ImageBitmaps a standalone
    // sibling cannot mint, and the worker-thread `onDone` form of every
    // heavy op. host_vision.h says why the ops are re-driven here instead of
    // wrapped. It also registers the per-frame drain and the shutdown hook,
    // once per process.
    installVisionHostOps(&engine);
#endif
#if BRO_WITH_FLORA
    broflora::api::installFlora();
    adoptGlobalProperty("FloraWorld");
#endif
    {
        // `bro.image` is ONE object with three sources: brokit's kernels
        // (which create it, now that `bro` is registered), the codecs and
        // ops from broimage_api, and `gpu`, which installImageGpuModule
        // mounts right after installBroRoots returns.
        installBrokitImageKernels();
        broimage::api::setPathResolver(&brokit::api::resolveAssetPath);
        broimage::api::installImage();
    }
}

// The Worker realm's share of the same list (host_natives.h). Every
// installer below is one whose state is per thread: a sibling's HostClass
// keeps its constructor and prototype per thread (each sibling's
// host_class.h), its install guards are thread_local, and bronze's native
// registry is per thread, so each call here builds the calling worker's own
// classes over the calling worker's own `bro` root and hands out nothing
// the main realm made. What is NOT here is everything that reaches the
// engine: broaudio (AudioContext, bro.mic), the listen tenants of
// brosoundml (wake / kws / sense / gesture / listen, which tap the engine's
// audio and inference scheduler) and the nav-mesh hooks (set once by the
// main realm; the process-global hook slots are already filled when a
// worker's bakeNavMesh reads them).
void installWorkerSiblingApis() {
#if BRO_WITH_GAMEAI
    brogameagent::api::installGameAi();
#endif
#if BRO_WITH_3D
    bromesh::api::setPathResolver(&brokit::api::resolveAssetPath);
    bromesh::api::installMesh();
    bromesh::api::installRigging();
#endif
#if BRO_WITH_TENSOR
    brotensor::api::setPathResolver(&brokit::api::resolveAssetPath);
    brotensor::api::installTensor();
    adoptGlobalProperty("GpuTensor");
#endif
#if BRO_WITH_LM
    brolm::api::setPathResolver(&brokit::api::resolveAssetPath);
    brolm::api::installLM();
    for (const char* name : {"AsyncHandle", "QwenTokenizer", "MistralTokenizer", "GemmaTokenizer",
                             "Llama3Tokenizer", "LMModel", "Qwen35Model", "Qwen3VLModel", "NllbModel",
                             "ClipModel", "T5Model", "LayaModel", "ModernBertModel", "Grammar"}) {
        adoptGlobalProperty(name);
    }
#endif
#if BRO_WITH_SOUNDML
    // The path resolver and log hook are process-global and already set by
    // the main realm's install; the audio engine and scheduler are not
    // consulted by the compute classes.
    brosoundml::api::installSoundMLCompute();
#endif
#if BRO_WITH_DIFFUSION
    brodiffusion::api::setPathResolver(&brokit::api::resolveAssetPath);
    brodiffusion::api::installDiffusion();
#endif
#if BRO_WITH_VISION
    brovisionml::api::setPathResolver(&brokit::api::resolveAssetPath);
    brovisionml::api::installVision();
    // No engine in a worker realm: the jobs this realm launches are drained
    // by tickWorkerSiblingApis below, not by the frame pump.
    installVisionHostOps(nullptr);
#endif
#if BRO_WITH_FLORA
    broflora::api::installFlora();
    adoptGlobalProperty("FloraWorld");
#endif
    installBrokitImageKernels();
    broimage::api::setPathResolver(&brokit::api::resolveAssetPath);
    broimage::api::installImage();
}

void tickWorkerSiblingApis() {
#if BRO_WITH_VISION
    // This thread's vision jobs only — the registry is per thread, because a
    // job's callbacks belong to the realm that launched it.
    tickVisionJobs();
#endif
#if BRO_WITH_LM
    brolm::api::tickLMAsync();
#endif
#if BRO_WITH_SOUNDML
    // This thread's async jobs only (a job's callbacks belong to the realm
    // that launched it).
    brosoundml::api::tickSoundMLAsync();
#endif
#if BRO_WITH_DIFFUSION
    brodiffusion::api::tickDiffusionAsync();
#endif
}

void shutdownWorkerSiblingApis() {
#if BRO_WITH_DIFFUSION
    // This thread's diffusion jobs: cancelled and joined while the worker's
    // realm (their callbacks' home) still exists.
    brodiffusion::api::shutdownDiffusionAsync();
#endif
}

}  // namespace bro::bronze_host
