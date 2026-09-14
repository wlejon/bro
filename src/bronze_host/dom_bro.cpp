#include "bronze_host/bronze_host.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_media.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/eval.h"
#include "engine/engine.h"
#include "util/asset_path.h"
#include "util/user_dirs.h"
#include "util/log.h"
#include "api/api.h"
#if BRO_WITH_3D
#include "bronze_host/host_scene_internal.h"
#endif

#include <filesystem>
#include <string>
#include <vector>
#include <span>

namespace bro::bronze_host {

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

Value makeGpuValue() {
    ObjectBuilder gpu;
    gpu.set("available", ev::fromBool(false));
    gpu.set("backend", ev::fromUtf8("cpu"));
    gpu.set("devices", hostArrayOf(1, [](size_t) { return ev::fromUtf8("cpu"); }));
    gpu.set("compiledBackends", hostArrayOf(1, [](size_t) { return ev::fromUtf8("cpu"); }));
    gpu.def("deviceName", 0, [](Value, std::span<const Value>) { return ev::null(); });
    gpu.def("deviceCount", 1, [](Value, std::span<const Value> a) {
        if (a.empty() || ev::toUtf8(a[0]) == "cpu") return ev::fromDouble(1);
        return ev::fromDouble(0);
    });
    gpu.def("memoryInfo", 0, [](Value, std::span<const Value>) { return ev::null(); });
    gpu.def("trim", 0, [](Value, std::span<const Value>) { return ev::fromBool(false); });
    return gpu.get();
}

static std::string getImageGpuJsPath() {
    std::error_code ec;
    if (const char* env = std::getenv("BRO_PROJECT_ROOT")) {
        auto p = std::filesystem::path(env) / "src/bronze_host/image_gpu.js";
        if (std::filesystem::exists(p, ec)) return std::filesystem::absolute(p, ec).string();
    }
    const auto exeDir = getExecutableDirectory();
    for (const auto& base : {
        exeDir,
        exeDir.parent_path(),
        exeDir.parent_path().parent_path(),
        std::filesystem::current_path(),
        std::filesystem::current_path().parent_path(),
    }) {
        for (const auto& rel : {
            "src/bronze_host/image_gpu.js",
            "image_gpu.js",
            "bronze/image_gpu.js",
        }) {
            auto p = base / rel;
            if (std::filesystem::exists(p, ec)) return std::filesystem::absolute(p, ec).string();
        }
    }
    return "src/bronze_host/image_gpu.js";
}

Value makeBroValue() {
    ObjectBuilder b;
    b.set("menu", makeBroMenuValue());
    b.set("window", makeBroWindowValue());
    b.set("settings", makeBroSettingsValue());
    b.set("math", makeBroMathValue());
    b.set("text", makeBroTextValue());
    b.set("steam", makeBroSteamValue());
    {
        ObjectBuilder time;
        time.accessor("scale",
            [](Value, std::span<const Value>) -> Value {
                auto* eng = hostEngine();
                return ev::fromDouble(eng ? eng->timeScale() : 1.0);
            },
            [](Value, std::span<const Value> a) -> Value {
                auto* eng = hostEngine();
                if (eng && !a.empty() && ev::isNumber(a[0])) {
                    eng->setTimeScale(ev::toDouble(a[0]));
                }
                return ev::undefined();
            });
        time.accessor("paused",
            [](Value, std::span<const Value>) -> Value {
                auto* eng = hostEngine();
                return ev::fromBool(eng ? eng->timePaused() : false);
            },
            [](Value, std::span<const Value> a) -> Value {
                auto* eng = hostEngine();
                if (eng && !a.empty()) {
                    eng->setTimePaused(ev::toBool(a[0]));
                }
                return ev::undefined();
            });
        time.accessor("now",
            [](Value, std::span<const Value>) -> Value {
                auto* eng = hostEngine();
                return ev::fromDouble(eng ? eng->timeNowMs() : hostClockMs());
            }, nullptr);
        b.set("time", time.get());
    }

    // App filesystem paths
    b.accessor("appDir",
        [](Value, std::span<const Value>) -> Value {
            auto* eng = hostEngine();
            std::string dir;
            if (eng && !eng->appDir().empty()) {
                dir = std::filesystem::path(eng->appDir()).make_preferred().string();
            }
            return ev::fromUtf8(dir);
        }, nullptr);

    b.accessor("userDataDir",
        [](Value, std::span<const Value>) -> Value {
            auto* eng = hostEngine();
            std::string base = eng ? eng->appDir() : "";
            std::string dir = util::appUserDataDir(base);
            if (dir.empty()) return ev::fromUtf8("");
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            return ev::fromUtf8(std::filesystem::path(dir).make_preferred().string());
        }, nullptr);

    b.def("resolvePath", 1,
        [](Value, std::span<const Value> a) -> Value {
            if (a.empty()) return ev::throwTypeError("resolvePath: path required");
            std::string s = ev::toUtf8(a[0]);
            std::string resolved = util::resolveAssetPath(s);
            return ev::fromUtf8(std::filesystem::path(resolved).make_preferred().string());
        });

    b.set("net", makeBroNetValue());
    b.set("mesh", makeBroMeshValue());
    b.set("image", makeBroImageValue());
    b.set("ai", makeBroAiValue());

    // Feature-gated stubs (GATED list from tests/_smoke_app/minimal_smoke.js)
    b.set("media", makeBroMediaValue());
    b.set("flora", makeUnavailableNamespace("flora", "BRO_WITH_FLORA"));
#if BRO_WITH_3D
    b.set("gizmo", makeBroGizmoValue());
#else
    b.set("gizmo", makeUnavailableNamespace("gizmo", "BRO_WITH_3D"));
#endif
    b.set("impostor", makeUnavailableNamespace("impostor", "BRO_WITH_3D"));
    b.set("lm", makeUnavailableNamespace("lm", "BRO_WITH_LM"));
    b.set("stt", makeUnavailableNamespace("stt", "BRO_WITH_SOUNDML"));
    b.set("tts", makeUnavailableNamespace("tts", "BRO_WITH_SOUNDML"));
    b.set("diar", makeUnavailableNamespace("diar", "BRO_WITH_SOUNDML"));
    b.set("rave", makeUnavailableNamespace("rave", "BRO_WITH_SOUNDML"));
    b.set("wake", makeUnavailableNamespace("wake", "BRO_WITH_SOUNDML"));
    b.set("kws", makeUnavailableNamespace("kws", "BRO_WITH_SOUNDML"));
    b.set("sense", makeUnavailableNamespace("sense", "BRO_WITH_SOUNDML"));
    b.set("gesture", makeUnavailableNamespace("gesture", "BRO_WITH_SOUNDML"));
    b.set("listen", makeUnavailableNamespace("listen", "BRO_WITH_SOUNDML"));
    b.set("mic", makeUnavailableNamespace("mic", "BRO_WITH_SOUNDML"));
    b.set("vision", makeUnavailableNamespace("vision", "BRO_WITH_VISION"));
    b.set("diffusion", makeUnavailableNamespace("diffusion", "BRO_WITH_DIFFUSION"));
    b.set("tensor", makeUnavailableNamespace("tensor", "BRO_WITH_TENSOR"));
    b.set("gpu", makeGpuValue());
    b.set("triposplat", makeUnavailableNamespace("triposplat", "BRO_WITH_TRIPOSPLAT"));
    b.set("motion", makeUnavailableNamespace("motion", "BRO_WITH_DIFFUSION+BRO_WITH_LM"));

    return b.get();
}

Value makeDunderBroValue() {
    ObjectBuilder b;

    // __bro.splash
    {
        ObjectBuilder splash;
        splash.def("dismiss", 0, [](Value, std::span<const Value>) -> Value {
            auto* eng = hostEngine();
            if (eng) eng->dismissSplash();
            return ev::undefined();
        });
        b.set("splash", splash.get());
    }

    // __bro.dismissSplash() alias
    b.def("dismissSplash", 0, [](Value, std::span<const Value>) -> Value {
        auto* eng = hostEngine();
        if (eng) eng->dismissSplash();
        return ev::undefined();
    });

    // __bro.viewport
    {
        ObjectBuilder vp;
        vp.accessor("width", [](Value, std::span<const Value>) -> Value {
            auto* eng = hostEngine();
            return ev::fromDouble(eng ? eng->viewportWidth() : 1920);
        }, nullptr);
        vp.accessor("height", [](Value, std::span<const Value>) -> Value {
            auto* eng = hostEngine();
            return ev::fromDouble(eng ? eng->viewportHeight() : 1080);
        }, nullptr);
        b.set("viewport", vp.get());
    }

    // __bro.perf
    {
        ObjectBuilder perf;
        perf.set("fps", ev::fromDouble(60.0));
        perf.set("frameTime", ev::fromDouble(16.6));
        perf.set("js", ev::fromDouble(0.0));
        perf.set("layout", ev::fromDouble(0.0));
        perf.set("raster", ev::fromDouble(0.0));
        perf.set("gpu", ev::fromDouble(0.0));
        perf.set("draw", ev::fromDouble(0.0));
        perf.set("windows", hostArrayOf(0, [](size_t) { return ev::undefined(); }));
        b.set("perf", perf.get());
    }

    // __bro.menu
    {
        ObjectBuilder menu;
        menu.def("getHeight", 0, [](Value, std::span<const Value>) -> Value {
            auto* eng = hostEngine();
            return ev::fromDouble(eng ? eng->menuBar().height : 28);
        });
        menu.def("getTree", 0, [](Value, std::span<const Value>) -> Value {
            auto* eng = hostEngine();
            if (!eng) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
            std::string json = eng->menuBar().toJSON();
            ev::CallResult r = ev::parseJson(json);
            return r.thrown ? hostArrayOf(0, [](size_t) { return ev::undefined(); }) : r.value;
        });
        menu.def("click", 1, [](Value, std::span<const Value> a) -> Value {
            auto* eng = hostEngine();
            if (eng && !a.empty()) {
                eng->triggerMenuAction(ev::toUtf8(a[0]));
            }
            return ev::undefined();
        });
        b.set("menu", menu.get());
    }

    // __bro.settingsUI
    {
        ObjectBuilder sUI;
        sUI.def("show", 1, [](Value, std::span<const Value> a) -> Value {
            auto* eng = hostEngine();
            if (eng && !a.empty()) {
                eng->showSystemPanel(ev::toUtf8(a[0]));
            }
            return ev::undefined();
        });
        sUI.def("getAllPanels", 0, [](Value, std::span<const Value>) -> Value {
            auto* eng = hostEngine();
            if (!eng) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
            std::vector<std::pair<std::string, std::string>> panels;
            for (const auto& d : eng->systemDocs()) {
                if (!d.tabLabel.empty()) panels.emplace_back(d.name, d.tabLabel);
            }
            return hostArrayOf(panels.size(), [&panels](size_t i) {
                ObjectBuilder item;
                item.set("name", ev::fromUtf8(panels[i].first));
                item.set("tabLabel", ev::fromUtf8(panels[i].second));
                return item.get();
            });
        });
        sUI.def("getActivePanel", 0, [](Value, std::span<const Value>) -> Value {
            auto* eng = hostEngine();
            return ev::fromUtf8(eng ? eng->systemActivePanel() : "");
        });
        sUI.def("toggle", 0, [](Value, std::span<const Value>) -> Value {
            auto* eng = hostEngine();
            if (eng) eng->toggleSystemSettings();
            return ev::undefined();
        });
        sUI.def("isVisible", 0, [](Value, std::span<const Value>) -> Value {
            auto* eng = hostEngine();
            return ev::fromBool(eng && eng->isSystemVisible());
        });
        sUI.def("getSettingsPanels", 0, [](Value, std::span<const Value>) -> Value {
            auto* eng = hostEngine();
            if (!eng) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
            std::vector<std::pair<std::string, std::string>> panels;
            for (const auto& d : eng->systemDocs()) {
                if (d.group == "settings") panels.emplace_back(d.name, d.tabLabel);
            }
            return hostArrayOf(panels.size(), [&panels](size_t i) {
                ObjectBuilder item;
                item.set("name", ev::fromUtf8(panels[i].first));
                item.set("label", ev::fromUtf8(panels[i].second));
                return item.get();
            });
        });
        sUI.def("getViewport", 0, [](Value, std::span<const Value>) -> Value {
            auto* eng = hostEngine();
            ObjectBuilder o;
            if (eng) {
                o.set("width", ev::fromDouble(eng->viewportWidth()));
                o.set("height", ev::fromDouble(eng->viewportHeight()));
                o.set("contentTop", ev::fromDouble(eng->contentTop()));
            }
            return o.get();
        });
        b.set("settingsUI", sUI.get());
    }

    // __bro.inspector
    {
        ObjectBuilder insp;
        insp.def("getLayout", 0, [](Value, std::span<const Value>) -> Value {
            auto* eng = hostEngine();
            ObjectBuilder o;
            if (eng) {
                const auto& i = eng->inspector();
                o.set("visible", ev::fromBool(i.visible));
                o.set("dock", ev::fromUtf8(i.dock == engine::InspectorDock::Right ? "right" : "bottom"));
                o.set("width", ev::fromDouble(i.width));
                o.set("height", ev::fromDouble(i.height));
                o.set("pickerMode", ev::fromBool(i.pickerMode));
                o.set("viewportWidth", ev::fromDouble(eng->viewportWidth()));
                o.set("viewportHeight", ev::fromDouble(eng->viewportHeight()));
                o.set("menuTop", ev::fromDouble(eng->menuBar().height));
            } else {
                o.set("visible", ev::fromBool(false));
                o.set("dock", ev::fromUtf8("right"));
                o.set("width", ev::fromDouble(320));
                o.set("height", ev::fromDouble(280));
                o.set("pickerMode", ev::fromBool(false));
                o.set("viewportWidth", ev::fromDouble(1920));
                o.set("viewportHeight", ev::fromDouble(1080));
                o.set("menuTop", ev::fromDouble(28));
            }
            return o.get();
        });
        insp.def("getAppTree", 0, [](Value, std::span<const Value>) -> Value {
            ObjectBuilder root;
            root.set("id", ev::fromDouble(0));
            root.set("tag", ev::fromUtf8("html"));
            root.set("idAttr", ev::fromUtf8(""));
            root.set("classes", ev::fromUtf8(""));
            root.set("hasChildren", ev::fromBool(false));
            root.set("children", hostArrayOf(0, [](size_t) { return ev::undefined(); }));
            return root.get();
        });
        insp.def("getAppChildren", 1, [](Value, std::span<const Value>) -> Value {
            return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        });
        insp.def("getSelected", 0, [](Value, std::span<const Value>) -> Value {
            return ev::null();
        });
        insp.def("select", 1, [](Value, std::span<const Value>) -> Value {
            return ev::undefined();
        });
        insp.def("setDock", 1, [](Value, std::span<const Value>) -> Value {
            return ev::undefined();
        });
        insp.def("setSize", 1, [](Value, std::span<const Value>) -> Value {
            return ev::undefined();
        });
        insp.def("setPickerMode", 1, [](Value, std::span<const Value>) -> Value {
            return ev::undefined();
        });
        insp.def("toggle", 0, [](Value, std::span<const Value>) -> Value {
            return ev::undefined();
        });
        b.set("inspector", insp.get());
    }

    return b.get();
}

void installBroGlobals(engine::Engine& engine) {
    Value broVal = makeBroValue();
    ev::registerGlobal("bro", broVal);
    Value dunderBroVal = makeDunderBroValue();
    ev::registerGlobal("__bro", dunderBroVal);
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        ev::setProperty(gt.value, "bro", broVal);
        ev::setProperty(gt.value, "__bro", dunderBroVal);
    }

    installNetSync(&engine);

    // Install bro.image kernels via brokit
    brokit::api::installImage();

    // Augment bro.image with transcodeKTX2 and lazy GPU colormap/fbm
    Value broObj = ev::getGlobal("bro");
    if (ev::isObject(broObj)) {
        Value imgVal = ev::getProperty(broObj, "image");
        if (ev::isObject(imgVal)) {
            Value codecImg = makeBroImageValue();
            for (const char* name : {"transcodeKTX2", "encodePngFile", "encodePng", "encodeJpegFile", "encodeJpeg"}) {
                Value fn = ev::getProperty(codecImg, name);
                if (!ev::isUndefined(fn)) {
                    ev::setProperty(imgVal, name, fn);
                }
            }

            static ev::Persistent s_imageGpuValue;
            static bool s_loadingImageGpu = false;

            ObjectBuilder img(imgVal);
            img.accessor("gpu",
                [](Value, std::span<const Value>) -> Value {
                    if (s_loadingImageGpu) {
                        return s_imageGpuValue.get();
                    }
                    if (ev::isUndefined(s_imageGpuValue.get())) {
                        s_loadingImageGpu = true;
                        auto* eng = hostEngine();
                        if (eng) {
                            std::string path = getImageGpuJsPath();
                            std::error_code ec;
                            if (std::filesystem::exists(path, ec)) {
                                evalScriptFile(*eng, path);
                            }
                        }
                        s_loadingImageGpu = false;
                    }
                    return s_imageGpuValue.get();
                },
                [](Value, std::span<const Value> a) -> Value {
                    if (!a.empty()) {
                        s_imageGpuValue.set(a[0]);
                    }
                    return ev::undefined();
                });
        }
    }
}

} // namespace bro::bronze_host
