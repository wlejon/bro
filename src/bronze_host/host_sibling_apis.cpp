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

#include "bronze_host/host_js_modules.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"
#include "util/log.h"

#if BRO_WITH_AUDIO
#include <broaudio/api.h>
#endif
#if BRO_WITH_GAMEAI
#include <brogameagent/api.h>
#include <brogameagent/types.h>
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
#if BRO_WITH_KERNEL
#include "bronze_host/host_kernel_gpu.h"
#include "bronze_host/host_tiles.h"
#include <brokernel/api.h>
#endif
#if BRO_WITH_3D
#include <bromesh/api.h>
#include "bronze_host/native_scene_internal.h"
#endif
#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#if BRO_WITH_CONF
#include <broconf/api.h>
#include <broconf/store.h>
#include <cstdlib>
#include <filesystem>
#endif
#if BRO_WITH_THEMES
#include <brothemes/api.h>
#endif
#if BRO_WITH_KEYS
#include <brokeys/api.h>
#endif
#if BRO_WITH_SEARCH
#include <brosearch/api.h>
#endif
#if BRO_WITH_APPS
#include <broapps/api.h>
#endif
#if BRO_WITH_VFS
#include <brovfs/api.h>
#endif
#if BRO_WITH_THUMB
#include <brothumb/api.h>
#include <brothumb/service.h>
#endif
#if BRO_WITH_SEAT
#include <broseat/api.h>
#endif
#if BRO_WITH_CRED
#include <brocred/api.h>
#endif
#if BRO_WITH_SYS
#include <brosys/api.h>
#endif
#if BRO_WITH_DISPLAYS
#include <brodisplays/api.h>
#endif
#if BRO_WITH_PORTAL
#include <broportal/api.h>
#endif
#if BRO_WITH_COMPOSITOR
#include <brocompositor/api.h>
#if defined(_WIN32)
#include <brocompositor/win/shell_backend.h>
#include "platform/window.h"
#endif
#endif
#if BRO_WITH_WL
#include <browl/api.h>
#endif
#if BRO_WITH_A11Y
#include <broa11y/api.h>
#endif
#if BRO_WITH_DECOR
#include <brodecor/api.h>
#endif
#if BRO_WITH_CLIP
#include <broclip/api.h>
#endif
#if BRO_WITH_MPRIS
#include <brompris/api.h>
#endif
#if BRO_WITH_PULSE
#include <bropulse/api.h>
#endif
#if BRO_WITH_IME
#include <broime/api.h>
#endif
#if BRO_WITH_REMOTE
#include "bronze_host/host_remote.h"
#endif
#include "api/api.h"  // brokit::api::resolveAssetPath
#include <broimage/api.h>
#include "render/image_codecs.h"

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
    auto readBound = [&](const char* key, float& out) -> bool {
        bronze::Value v = bronze::embed::getProperty(boundsRoot.get(), key);
        if (!bronze::embed::isNumber(v)) return false;
        const double d = bronze::embed::toDouble(v);
        if (!std::isfinite(d)) return false;
        out = static_cast<float>(d);
        return true;
    };
    float minX = 0.0f, minZ = 0.0f, maxX = 0.0f, maxZ = 0.0f;
    if (!readBound("minX", minX) || !readBound("minZ", minZ) ||
        !readBound("maxX", maxX) || !readBound("maxZ", maxZ)) {
        err = "bakeNavMesh: terrainBounds requires finite minX, minZ, maxX, maxZ";
        return false;
    }
    auto readRootFloat = [&](const char* key, float def) -> float {
        bronze::Value v = bronze::embed::getProperty(rootVal, key);
        if (!bronze::embed::isNumber(v)) return def;
        const double d = bronze::embed::toDouble(v);
        return std::isfinite(d) ? static_cast<float>(d) : def;
    };
    float step = readRootFloat("terrainStep", 1.0f);
    if (step <= 0.0f) step = 1.0f;
    float rayStart = readRootFloat("terrainRayStart", 100.0f);
    float rayLength = readRootFloat("terrainRayLength", 200.0f);

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
    static engine::Engine* s_activeEngine = nullptr;
    const bool installHooks = (s_activeEngine != &engine);
    s_activeEngine = &engine;
    if (installHooks) {
        engine.addShutdownHook([eng = &engine] {
            if (s_activeEngine == eng) s_activeEngine = nullptr;
        });
    }
#if BRO_WITH_AUDIO
    // AudioContext and the node classes need no root; bro.mic mounts onto
    // the `bro` root and re-sets the global to the object it found.
    // The engine's audio is opened on a script's first use of it, so a page
    // that never plays a sound never opens the audio device.
    broaudio::api::setAudioEngineProvider([]() -> broaudio::Engine* {
        return s_activeEngine ? s_activeEngine->audioEngine() : nullptr;
    });
    // Its file loaders, savers and preset IO take paths the way fs.* does.
    broaudio::api::setPathResolver(&brokit::api::resolveAssetPath);
    broaudio::api::installAudio();
    broaudio::api::installMic();
    // bro.ear (measure / compare / spectrogram) adds to `bro.ear` rather than
    // replacing it; brosoundml mounts loadClap onto the same object below.
    broaudio::api::installEar();
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
    installSdfGraphGpuExtensions();
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
    if (installHooks) {
        // The LM tick fires generate() callbacks and settles LayaModel
        // promises (predictAsync / loadLayaAsync). Their reactions run right
        // after, in this frame, before it renders: a result the device
        // finished mid-frame reaches the page one frame sooner than waiting
        // for the next frame's microtask checkpoint. The shutdown hook stops
        // every Laya scheduler's device threads before the runtime and
        // brotensor go away.
        engine.addFramePump([] {
            brolm::api::tickLMAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { brolm::api::shutdownLM(); });
    }
#endif
#if BRO_WITH_SOUNDML
    // One installer covers stt, tts, diar, rave, wake, kws, sense, gesture
    // and listen. Its async jobs (background loads / inferences) deliver
    // their JS callbacks from the engine's frame pump, and are cancelled +
    // joined at shutdown before the runtime and brotensor go away.
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
    brosoundml::api::setAudioEngineProvider([]() -> broaudio::Engine* {
        return s_activeEngine ? s_activeEngine->audioEngine() : nullptr;
    });
    {
        // The AudioInference worker is made when the first stream asks for
        // a pump, not for every page.
        brosoundml::api::InferenceScheduler sched;
        sched.addPump = [](std::function<void()> pump) -> std::uint32_t {
            engine::AudioInference* inference = s_activeEngine ? s_activeEngine->audioInference() : nullptr;
            return inference ? inference->addPump(std::move(pump)) : 0;
        };
        sched.removePump = [](std::uint32_t id) {
            if (auto* inference = s_activeEngine ? s_activeEngine->existingAudioInference() : nullptr)
                inference->removeTask(id);
        };
        sched.threaded = [] {
            engine::AudioInference* inference = s_activeEngine ? s_activeEngine->audioInference() : nullptr;
            return inference && inference->threaded();
        };
        brosoundml::api::setInferenceScheduler(std::move(sched));
    }
    brosoundml::api::installSoundML();
    if (installHooks) {
        engine.addFramePump([] { brosoundml::api::tickSoundML(); });
        engine.addShutdownHook([] { brosoundml::api::shutdownSoundML(); });
    }
#endif
#if BRO_WITH_DIFFUSION
    // bro.diffusion and bro.triposplat together.
    brodiffusion::api::setPathResolver(&brokit::api::resolveAssetPath);
    brodiffusion::api::installDiffusion();
    if (installHooks) {
        // A background generate (generateAsync, or generate/imageToImage/
        // inpaint with onDone or async: true) delivers its onDone from this
        // tick; without it the result waits for a script that calls
        // bro.diffusion.tick() itself. Their reactions run in the same
        // frame, as the LM tick's do. Ungated by bro.time pause, like every
        // frame pump. The shutdown hook cancels and joins the jobs still
        // running before the runtime and brotensor go away.
        engine.addFramePump([] {
            brodiffusion::api::tickDiffusionAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { brodiffusion::api::shutdownDiffusionAsync(); });
    }
#endif
#if BRO_WITH_VISION
    brovisionml::api::setPathResolver(&brokit::api::resolveAssetPath);
    brovisionml::api::installVision();
    // ...then bro's half: the `image` / `matte` ImageBitmaps a standalone
    // sibling cannot mint, and the worker-thread `onDone` form of every
    // heavy op. host_vision.h says why the ops are re-driven here instead of
    // wrapped. It also registers the per-frame drain and the shutdown hook.
    installVisionHostOps(&engine);
#endif
#if BRO_WITH_FLORA
    broflora::api::installFlora();
    adoptGlobalProperty("FloraWorld");
#endif
#if BRO_WITH_KERNEL
    // bro's half first: the engine's GPU device, which installKernel mounts
    // as the GPU half (host_kernel_gpu.h).
    installKernelGpu(engine);
    brokernel::api::installKernel();
    installKernelBackground();
    installTiles(engine);
#endif
    {
        // `bro.image` is ONE object with two sources: the kernels, codecs
        // and ops from broimage_api (which creates it, now that `bro` is
        // registered), and `gpu`, which installImageGpuModule mounts right
        // after installBroRoots returns. brokit carries a copy of the
        // kernels too; broimage's are the ones installed.
        broimage::api::setPathResolver(&brokit::api::resolveAssetPath);
        // WebP and SVG read by broimage — bro.thumb — as by <img>.
        render::registerImageCodecs();
        broimage::api::installImage();
    }
#if BRO_WITH_CONF
    {
        // BRO_APP_HOME (an app's tests, a scratch profile) holds everything the app keeps,
        // the desktop settings it reads and writes through bro.conf included: never the
        // user's own settings.ini, and no session bus to hear the real desktop's changes.
        static bool confStoreChosen = false;
        const char* appHome = std::getenv("BRO_APP_HOME");
        if (!confStoreChosen && appHome && *appHome) {
            broconf::StoreOptions opts;
            opts.user_config_path = std::filesystem::absolute(std::filesystem::u8path(appHome)) / "config" / "settings.ini";
            opts.enable_dbus = false;
            broconf::api::setStore(broconf::Store::create(std::move(opts)));
        }
        confStoreChosen = true;
    }
    broconf::api::installConf();
    if (installHooks) {
        engine.addFramePump([] {
            broconf::api::tickConfAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { broconf::api::shutdownConfAsync(); });
    }
#endif
#if BRO_WITH_THEMES
    bro::themes::api::setPathResolver(&brokit::api::resolveAssetPath);
    bro::themes::api::installThemes();
#endif
#if BRO_WITH_KEYS
    bro::keys::api::installKeys();
#endif
#if BRO_WITH_SEARCH
    brosearch::api::installSearch();
    if (installHooks) {
        engine.addFramePump([] {
            brosearch::api::tickSearchAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { brosearch::api::shutdownSearchAsync(); });
    }
#endif
#if BRO_WITH_APPS
    broapps::api::installApps();
    if (installHooks) {
        engine.addFramePump([] {
            broapps::api::tickAppsAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { broapps::api::shutdownAppsAsync(); });
    }
#endif
#if BRO_WITH_VFS
    brovfs::api::installVfs();
    if (installHooks) {
        engine.addFramePump([] {
            brovfs::api::tickVfsAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { brovfs::api::shutdownVfsAsync(); });
    }
#endif
#if BRO_WITH_THUMB
    {
        // BRO_APP_HOME (an app's tests, a scratch profile) holds the thumbnail
        // cache too, as it holds bro.conf's store: a headless test's previews
        // land under <home>/cache/thumbnails, never in the user's real
        // %LOCALAPPDATA%\thumbnails or ~/.cache/thumbnails. Chosen once per
        // process, before the first realm mounts bro.thumb.
        static bool thumbServiceChosen = false;
        const char* appHome = std::getenv("BRO_APP_HOME");
        if (!thumbServiceChosen && appHome && *appHome) {
            brothumb::ThumbnailService::Config cfg;
            cfg.cache_dir =
                std::filesystem::absolute(std::filesystem::u8path(appHome)) / "cache" / "thumbnails";
            std::string err;
            if (auto svc = brothumb::ThumbnailService::create(cfg, &err))
                brothumb::api::setService(std::move(svc));
            else
                LOG_WARN("bro.thumb: no service under BRO_APP_HOME (%s)", err.c_str());
        }
        thumbServiceChosen = true;
    }
    brothumb::api::installThumb();
    if (installHooks) {
        engine.addFramePump([] {
            brothumb::api::tickThumbAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { brothumb::api::shutdownThumbAsync(); });
    }
#endif
#if BRO_WITH_SEAT
    broseat::api::installSeat();
    if (installHooks) {
        // The idle timer (bro.seat.setIdleTimeout) runs on the engine's
        // record of user input: under DRM the engine reads the devices
        // itself, and a compositor client's idle-inhibit holds it off.
        engine.addFramePump([eng = &engine] {
            if (s_activeEngine == eng) {
                broseat::api::tickIdle(eng->activityClockMs(), eng->lastUserActivityMs(),
                                       eng->hostIdleInhibited());
            }
            broseat::api::tickSeatAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { broseat::api::shutdownSeatAsync(); });
    }
#endif
#if BRO_WITH_CRED
    brocred::api::installCred();
    if (installHooks) {
        engine.addFramePump([] {
            brocred::api::tickCredAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { brocred::api::shutdownCredAsync(); });
    }
#endif
#if BRO_WITH_SYS
    brosys::api::installSys();
    if (installHooks) {
        engine.addFramePump([] {
            brosys::api::tickSysAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { brosys::api::shutdownSysAsync(); });
    }
#endif
#if BRO_WITH_DISPLAYS
    brodisplays::api::installDisplays();
    if (installHooks) {
        engine.addFramePump([] {
            brodisplays::api::tickDisplaysAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { brodisplays::api::shutdownDisplaysAsync(); });
    }
#endif
#if BRO_WITH_PORTAL
    broportal::api::installPortal();
    if (installHooks) {
        engine.addFramePump([] {
            broportal::api::tickPortalAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { broportal::api::shutdownPortalAsync(); });
    }
#endif
#if BRO_WITH_COMPOSITOR
    brocompositor::api::installCompositor();
    if (installHooks) {
#if defined(_WIN32)
        static std::unique_ptr<brocompositor::win::ShellBackend> s_winShellBackend;
        if (engine.displayMode() != engine::DisplayMode::Headless) {
            brocompositor::win::ShellConfig sCfg;
            sCfg.recover = true;
            sCfg.report_existing = true;
            std::string sErr;
            s_winShellBackend = brocompositor::win::ShellBackend::create(sCfg, &sErr);
            if (!s_winShellBackend) {
                LOG_WARN("brocompositor: win::ShellBackend::create failed: %s", sErr.c_str());
            } else {
                auto eqPtr = std::shared_ptr<brocompositor::EventQueue>(
                    &s_winShellBackend->events(), [](brocompositor::EventQueue*) {});
                brocompositor::api::setEventQueue(eqPtr);
                brocompositor::api::setCommandSink(
                    [](const std::vector<brocompositor::Command>& cmds) -> size_t {
                        if (s_winShellBackend) return s_winShellBackend->execute(cmds);
                        return cmds.size();
                    });
                brocompositor::api::setWindowIconSource(
                    [](brocompositor::WindowId id, int32_t size) -> std::optional<brocompositor::WindowIcon> {
                        if (s_winShellBackend) return s_winShellBackend->window_icon(id, size);
                        return std::nullopt;
                    });
                // The session's shell in place of Explorer (helm as the
                // Winlogon shell): its window is the shell window, kept
                // beneath the applications until an overlay raises it,
                // and its panels reserve the work area every application
                // sees (bro.compositor.setShellOverlay / reserveEdge).
                if (engine.isDesktopShell()) {
                    brocompositor::api::ShellRole role;
                    role.is_desktop_shell = [] { return s_winShellBackend && s_winShellBackend->is_desktop_shell(); };
                    role.set_overlay = [](bool on) {
                        return s_winShellBackend && s_winShellBackend->set_shell_raised(on);
                    };
                    role.reserve_edge = [](brocompositor::MonitorId monitor, brocompositor::Edge edge,
                                           int32_t thickness) {
                        return s_winShellBackend ? s_winShellBackend->reserve_edge(monitor, edge, thickness)
                                                 : brocompositor::kNoReservation;
                    };
                    role.release_edge = [](brocompositor::ReservationId id) {
                        return s_winShellBackend && s_winShellBackend->release_edge(id);
                    };
                    brocompositor::api::setShellRole(std::move(role));
                }
            }
        }
#endif
        engine.addFramePump([eng = &engine] {
#if defined(_WIN32)
            // Attached on the window's own thread (this one), once it exists.
            static bool s_shellAttached = false;
            if (!s_shellAttached && s_winShellBackend && s_activeEngine == eng &&
                eng->isDesktopShell() && eng->window()) {
                if (void* hwnd = eng->window()->nativeHandle().hwnd) {
                    s_shellAttached = true;
                    const bool ok = s_winShellBackend->attach_desktop_shell(
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hwnd)));
                    LOG_INFO("brocompositor: desktop shell window %p attached=%d registered=%d", hwnd, ok ? 1 : 0,
                             s_winShellBackend->is_desktop_shell() ? 1 : 0);
                }
            }
#else
            (void)eng;
#endif
            brocompositor::api::tickCompositorAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] {
            brocompositor::api::shutdownCompositorAsync();
#if defined(_WIN32)
            brocompositor::api::setShellRole({});
            brocompositor::api::setWindowIconSource({});
            s_winShellBackend.reset();
#endif
        });
    }
#endif
#if BRO_WITH_WL
    browl::api::setAutoConnect(engine.displayMode() != engine::DisplayMode::Headless);
    browl::api::installWl();
    if (installHooks) {
        engine.addFramePump([] {
            browl::api::tickWlAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { browl::api::shutdownWlAsync(); });
    }
#endif
#if BRO_WITH_A11Y
    broa11y::api::installA11y();
    if (installHooks) {
        engine.addFramePump([] {
            broa11y::api::tickA11yAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { broa11y::api::shutdownA11yAsync(); });
    }
#endif
#if BRO_WITH_DECOR
    brodecor::api::installDecor();
    if (installHooks) {
        engine.addFramePump([] {
            brodecor::api::tickDecorAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { brodecor::api::shutdownDecorAsync(); });
    }
#endif
#if BRO_WITH_CLIP
    broclip::api::installClip();
    if (installHooks) {
        engine.addFramePump([] {
            broclip::api::tickClipAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { broclip::api::shutdownClipAsync(); });
    }
#endif
#if BRO_WITH_MPRIS
    // Headless sees no session players and publishes none onto the session
    // bus: a test must not read or answer to the machine's media players.
    brompris::api::setSessionAccess(engine.displayMode() != engine::DisplayMode::Headless);
    brompris::api::installMpris();
    if (installHooks) {
        engine.addFramePump([] {
            brompris::api::tickMprisAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { brompris::api::shutdownMprisAsync(); });
    }
#endif
#if BRO_WITH_PULSE
    bropulse::api::installPulse();
    if (installHooks) {
        engine.addFramePump([] {
            bropulse::api::tickPulseAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { bropulse::api::shutdownPulseAsync(); });
    }
#endif
#if BRO_WITH_IME
    bro::ime::api::installIme();
    if (installHooks) {
        engine.addFramePump([] {
            bro::ime::api::tickImeAsync();
            if (ev::microtasksPending()) ev::drainMicrotasks();
        });
        engine.addShutdownHook([] { bro::ime::api::shutdownImeAsync(); });
    }
#endif
#if BRO_WITH_REMOTE
    // bro.remote, with its frame pump and shutdown hook (host_remote.cpp).
    installRemoteHost(engine);
#endif
}

void shutdownSiblingApisForReload() {
#if BRO_WITH_LM
    brolm::api::shutdownLM();
#endif
#if BRO_WITH_SOUNDML
    brosoundml::api::shutdownSoundML();
#endif
#if BRO_WITH_DIFFUSION
    brodiffusion::api::shutdownDiffusionAsync();
#endif
#if BRO_WITH_VISION
    shutdownVisionJobs();
#endif
#if BRO_WITH_CONF
    broconf::api::shutdownConfAsync();
#endif
#if BRO_WITH_SEARCH
    brosearch::api::shutdownSearchAsync();
#endif
#if BRO_WITH_APPS
    broapps::api::shutdownAppsAsync();
#endif
#if BRO_WITH_VFS
    brovfs::api::shutdownVfsAsync();
#endif
#if BRO_WITH_THUMB
    brothumb::api::shutdownThumbAsync();
#endif
#if BRO_WITH_SEAT
    broseat::api::shutdownSeatAsync();
#endif
#if BRO_WITH_CRED
    brocred::api::shutdownCredAsync();
#endif
#if BRO_WITH_SYS
    brosys::api::shutdownSysAsync();
#endif
#if BRO_WITH_DISPLAYS
    brodisplays::api::shutdownDisplaysAsync();
#endif
#if BRO_WITH_PORTAL
    broportal::api::shutdownPortalAsync();
#endif
#if BRO_WITH_WL
    browl::api::shutdownWlAsync();
#endif
#if BRO_WITH_A11Y
    broa11y::api::shutdownA11yAsync();
#endif
#if BRO_WITH_DECOR
    brodecor::api::shutdownDecorAsync();
#endif
#if BRO_WITH_CLIP
    broclip::api::shutdownClipAsync();
#endif
#if BRO_WITH_MPRIS
    brompris::api::shutdownMprisAsync();
#endif
#if BRO_WITH_PULSE
    bropulse::api::shutdownPulseAsync();
#endif
#if BRO_WITH_IME
    bro::ime::api::shutdownImeAsync();
#endif
}

// The Worker realm's share of the same list (host_natives.h). Every
// installer below is one whose state is per thread: a sibling's HostClass
// keeps its constructor and prototype per thread (each sibling's
// host_class.h), its install guards are thread_local, and bronze's native
// registry is per thread, so each call here builds the calling worker's own
// classes over the calling worker's own `bro` root and hands out nothing
// the main realm made. What is NOT here is everything that reaches the
// engine: broaudio's AudioContext and bro.mic (bro.ear and SynthGraph, which
// are pure computation, are here; a worker renders a graph offline but cannot
// play one), the listen tenants of
// brosoundml (wake / kws / sense / gesture / listen, which tap the engine's
// audio and inference scheduler) and the nav-mesh hooks (set once by the
// main realm; the process-global hook slots are already filled when a
// worker's bakeNavMesh reads them).
void installWorkerSiblingApis() {
#if BRO_WITH_AUDIO
    broaudio::api::installEar();
    broaudio::api::installSynthGraph();
#endif
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
#if BRO_WITH_KERNEL
    brokernel::api::installKernel();
#endif
    broimage::api::setPathResolver(&brokit::api::resolveAssetPath);
    render::registerImageCodecs();
    broimage::api::installImage();
#if BRO_WITH_THEMES
    bro::themes::api::setPathResolver(&brokit::api::resolveAssetPath);
    bro::themes::api::installThemes();
#endif
#if BRO_WITH_KEYS
    bro::keys::api::installKeys();
#endif
#if BRO_WITH_SEARCH
    brosearch::api::installSearch();
#endif
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
#if BRO_WITH_SEARCH
    brosearch::api::tickSearchAsync();
#endif
}

void shutdownWorkerSiblingApis() {
#if BRO_WITH_DIFFUSION
    // This thread's diffusion jobs: cancelled and joined while the worker's
    // realm (their callbacks' home) still exists.
    brodiffusion::api::shutdownDiffusionAsync();
#endif
#if BRO_WITH_SEARCH
    brosearch::api::shutdownSearchAsync();
#endif
}

}  // namespace bro::bronze_host
