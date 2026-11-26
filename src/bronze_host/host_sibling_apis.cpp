// Sibling library JavaScript API installers (called once per Bronze realm after roots).
// Each installer mounts once onto `bro` / `__bro_native` before registerBroNatives;
// none are re-entrant. Header comments trimmed for file-size compliance; order:
//   brotensor before ML inference stacks; installKernelGpu/Background/Tiles with kernel.

#include "bronze_host/host_js_modules.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"
#include "util/log.h"
#include "util/main_loop_wake.h"

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
#if BRO_WITH_TENSOR
#include "bronze_host/host_kernel_tensor.h"
#endif
#include "bronze_host/host_tiles.h"
#include <brokernel/api.h>
#if BRO_WITH_3D
#include "bronze_host/host_voxeldag.h"
#endif
#endif
#if BRO_WITH_3D
#include <bromesh/api.h>
#include "bronze_host/native_scene_internal.h"
#endif
#if BRO_WITH_DYNAMICS
#include "dynamics/world.h"
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
#elif defined(__APPLE__)
#include <brocompositor/mac/shell_backend.h>
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

#if BRO_WITH_DYNAMICS
// A bake's `fromDynamics` (a bro.dynamics World) and `dynamicsMask` (the layers of its
// fixed bodies to read, all when absent).
dynamics::World* bakeWorld(bronze::Value rootVal, uint32_t& mask) {
    bronze::embed::Persistent root(rootVal);
    dynamics::World* world = dynamicsWorld(dynamicsWorldId(bronze::embed::getProperty(root.get(), "fromDynamics")));
    mask = 0xffffffffu;
    bronze::Value mv = bronze::embed::getProperty(root.get(), "dynamicsMask");
    if (bronze::embed::isNumber(mv)) mask = static_cast<uint32_t>(static_cast<int64_t>(bronze::embed::toDouble(mv)));
    return world;
}
#endif

bool collectNavGeometry(bronze::Value rootVal, std::vector<float>& xyz,
                        std::vector<uint32_t>& indices, std::string& err) {
#if BRO_WITH_DYNAMICS
    {
        uint32_t mask = 0;
        if (dynamics::World* world = bakeWorld(rootVal, mask)) world->fixedTriangles(mask, xyz, indices);
    }
#endif

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
#if BRO_WITH_DYNAMICS
    {
        uint32_t mask = 0;
        if (dynamics::World* dyn = bakeWorld(rootVal, mask)) {
            float lo = -1e9f, hi = 1e9f;
            bronze::Value v = bronze::embed::getProperty(rootVal, "dynamicsMinY");
            if (bronze::embed::isNumber(v)) lo = static_cast<float>(bronze::embed::toDouble(v));
            v = bronze::embed::getProperty(rootVal, "dynamicsMaxY");
            if (bronze::embed::isNumber(v)) hi = static_cast<float>(bronze::embed::toDouble(v));
            for (const dynamics::Bounds& b : dyn->fixedBounds(mask)) {
                if (b.max[1] < lo || b.min[1] > hi) continue;
                // A floor under the whole grid blocks nothing.
                if (b.min[0] <= minX && b.max[0] >= maxX && b.min[2] <= minZ && b.max[2] >= maxZ) continue;
                outBoxes.push_back(brogameagent::AABB{0.5f * (b.min[0] + b.max[0]), 0.5f * (b.min[2] + b.max[2]),
                                                      0.5f * (b.max[0] - b.min[0]), 0.5f * (b.max[2] - b.min[2])});
            }
        }
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
    if (!s_activeEngine) s_activeEngine = &engine;
    if (installHooks) {
        engine.addShutdownHook([&engine] {
            if (s_activeEngine == &engine) s_activeEngine = nullptr;
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
#if BRO_WITH_TENSOR
    installKernelTensorInterop();
#endif
    installTiles(engine);
#if BRO_WITH_3D
    installVoxelDag(engine);
#endif
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
        // A finished thumbnail wakes a window idling between frames, which
        // would otherwise settle its promise only at the next input.
        brothumb::api::setWake(&util::wakeMainLoop);
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
        // Headless, on its virtual clock, the session's other programs'
        // inhibitors are none of its business.
        engine.addFramePump([eng = &engine] {
            if (s_activeEngine == eng) {
                broseat::api::tickIdle(eng->activityClockMs(), eng->lastUserActivityMs(),
                                       eng->hostIdleInhibited(),
                                       eng->displayMode() != engine::DisplayMode::Headless);
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
            brocompositor::win::ShellConfig sCfg; sCfg.recover = true; sCfg.report_existing = true;
            std::string sErr;
            s_winShellBackend = brocompositor::win::ShellBackend::create(sCfg, &sErr);
            if (!s_winShellBackend) {
                LOG_WARN("brocompositor: win::ShellBackend::create failed: %s", sErr.c_str());
            } else {
                brocompositor::api::setEventQueue(std::shared_ptr<brocompositor::EventQueue>(
                    &s_winShellBackend->events(), [](brocompositor::EventQueue*) {}));
                brocompositor::api::setCommandSink([](const std::vector<brocompositor::Command>& cmds) -> size_t {
                    return s_winShellBackend ? s_winShellBackend->execute(cmds) : cmds.size();
                });
                brocompositor::api::setWindowIconSource(
                    [](brocompositor::WindowId id, int32_t size) -> std::optional<brocompositor::WindowIcon> {
                        return s_winShellBackend ? s_winShellBackend->window_icon(id, size) : std::nullopt;
                    });
                // Session shell (Explorer replacement): stays beneath apps until an
                // overlay raises it, and reserves work-area edges.
                if (engine.isDesktopShell()) {
                    brocompositor::api::ShellRole role;
                    role.is_desktop_shell = [] { return s_winShellBackend && s_winShellBackend->is_desktop_shell(); };
                    role.set_overlay = [](bool on) { return s_winShellBackend && s_winShellBackend->set_shell_raised(on); };
                    role.reserve_edge = [](brocompositor::MonitorId m, brocompositor::Edge e, int32_t t) {
                        return s_winShellBackend ? s_winShellBackend->reserve_edge(m, e, t) : brocompositor::kNoReservation;
                    };
                    role.release_edge = [](brocompositor::ReservationId id) { return s_winShellBackend && s_winShellBackend->release_edge(id); };
                    brocompositor::api::setShellRole(std::move(role));
                }
            }
        }
#elif defined(__APPLE__)
        static std::unique_ptr<brocompositor::mac::ShellBackend> s_macShellBackend;
        if (engine.displayMode() != engine::DisplayMode::Headless) {
            std::string sErr;
            brocompositor::mac::ShellConfig sCfg; sCfg.recover = true; sCfg.report_existing = true;
            s_macShellBackend = brocompositor::mac::ShellBackend::create(sCfg, &sErr);
            if (!s_macShellBackend) {
                LOG_WARN("brocompositor: mac::ShellBackend::create failed: %s", sErr.c_str());
            } else {
                brocompositor::api::setEventQueue(std::shared_ptr<brocompositor::EventQueue>(
                    &s_macShellBackend->events(), [](brocompositor::EventQueue*) {}));
                brocompositor::api::setCommandSink([](const std::vector<brocompositor::Command>& cmds) -> size_t {
                    return s_macShellBackend ? s_macShellBackend->execute(cmds) : cmds.size();
                });
                if (engine.isDesktopShell()) {
                    brocompositor::api::ShellRole role;
                    role.is_desktop_shell = [] { return s_macShellBackend != nullptr; };
                    role.reserve_edge = [](brocompositor::MonitorId m, brocompositor::Edge e, int32_t t) {
                        return s_macShellBackend ? s_macShellBackend->reserve_edge(m, e, t) : brocompositor::kNoReservation;
                    };
                    role.release_edge = [](brocompositor::ReservationId id) { return s_macShellBackend && s_macShellBackend->release_edge(id); };
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
#elif defined(__APPLE__)
            brocompositor::api::setShellRole({});
            s_macShellBackend.reset();
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

}  // namespace bro::bronze_host
