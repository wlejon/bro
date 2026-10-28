// The scene's background work shown in the system panels: the splash's line of
// what is still being made (the page's script compile, the world shader sets
// compiling, the pipelines building), and after the splash the corner status
// panel (system/status.html) that shows the same while it lasts and fades out
// when it is done (docs/system-panels.md, docs/compile-progress.md).
//
// The counts are the scene's: shader_warmup::progress() (every world shader
// set asked for in this process, the warm-up record's included) and
// pipeline_async::inFlight() (graphics pipelines built off the frame thread),
// the same that scene.pendingWork() reports.

#include "engine/engine.h"

#include "dom/document.h"
#include "dom/element.h"
#if BRO_WITH_3D
#include "scene/vulkan/frame/scene_shader_warmup.h"
#endif

#include <cmath>
#include <string>

#if BRO_WITH_3D
// (Declared in scene_vk_target_format.h, which needs the scene's Vulkan
// function table; this is the one function the engine reads from it.)
namespace bro::scene::vk::pipeline_async {
int inFlight();
}
#endif

namespace bro::engine {

namespace {

// The scene's counts; a build without the scene compiles nothing.
struct SceneWork {
    int shaders = 0, shadersDone = 0, building = 0;
};

SceneWork sceneWork() {
    SceneWork w;
#if BRO_WITH_3D
    const auto sp = scene::vk::shader_warmup::progress();
    w.shaders = sp.shaders;
    w.shadersDone = sp.shadersDone;
    w.building = scene::vk::pipeline_async::inFlight();
#endif
    return w;
}

std::string workText(bool compiling, double compileProgress) {
    const SceneWork sp = sceneWork();
    const int building = sp.building;
    std::string out;
    auto add = [&](const std::string& s) {
        if (!out.empty()) out += "   ";
        out += s;
    };
    if (compiling && compileProgress >= 0.0)
        add("compiling scripts " + std::to_string(static_cast<int>(std::lround(compileProgress * 100.0))) + "%");
    if (sp.shadersDone < sp.shaders)
        add("compiling shaders " + std::to_string(sp.shadersDone) + " / " + std::to_string(sp.shaders));
    if (building > 0) add("building pipelines " + std::to_string(building));
    return out;
}

SystemDocument* findGroup(std::vector<SystemDocument>& docs, const char* group) {
    for (auto& d : docs)
        if (d.group == group) return &d;
    return nullptr;
}

// Writes `text` into the panel's #bro-work, when it changed.
void writeWork(SystemDocument* doc, std::string& last, const std::string& text) {
    if (!doc || !doc->document || last == text) return;
    dom::Element* el = doc->document->getElementById("bro-work");
    if (!el) return;
    el->setTextContent(text);
    doc->document->markDirty();
    last = text;
}

}  // namespace

bool Engine::backgroundWorkPending() const {
    const SceneWork sp = sceneWork();
    return sp.shadersDone < sp.shaders || sp.building > 0;
}

void Engine::tickWorkStatus(double nowMs) {
    if (splashVisible_) {
        SystemDocument* splash = findGroup(systemDocs_, "splash");
        writeWork(splash, splashWorkText_, workText(appCompiling_, compileProgress_));
        return;
    }
    if (!statusEnabled_) return;

    // Work that lasts a moment (a pipeline or two made as a view turns) shows
    // nothing; work still under way as the splash goes shows at once.
    constexpr double kShowAfterMs = 250.0;
    constexpr double kHideAfterMs = 900.0;   // the box's fade (status.html) and a margin
    const bool busy = backgroundWorkPending();
    if (busy) {
        statusIdleSinceMs_ = -1.0;
        if (statusBusySinceMs_ < 0.0) statusBusySinceMs_ = splashReadyMs_ >= 0.0 ? nowMs - kShowAfterMs : nowMs;
        splashReadyMs_ = -1.0;
        if (!statusVisible_ && nowMs - statusBusySinceMs_ >= kShowAfterMs) {
            statusVisible_ = true;
            systemDirty_ = true;
            return;  // built at the top of the next frame (loadVisibleSystemPanels)
        }
    } else {
        statusBusySinceMs_ = -1.0;
        splashReadyMs_ = -1.0;
    }
    if (!statusVisible_) return;

    SystemDocument* status = findGroup(systemDocs_, "status");
    dom::Element* root = status && status->document ? status->document->documentElement() : nullptr;
    if (!root) return;
    if (busy) {
        writeWork(status, statusWorkText_, workText(false, -1.0));
        if (!statusBusy_) {
            root->setAttribute("bro-busy", "");
            status->document->markDirty();
            statusBusy_ = true;
        }
        return;
    }
    if (statusBusy_) {
        root->removeAttribute("bro-busy");
        status->document->markDirty();
        statusBusy_ = false;
        statusIdleSinceMs_ = nowMs;
    }
    if (statusIdleSinceMs_ < 0.0) statusIdleSinceMs_ = nowMs;
    if (nowMs - statusIdleSinceMs_ >= kHideAfterMs) {
        statusVisible_ = false;
        statusIdleSinceMs_ = -1.0;
        systemDirty_ = true;
    }
}

}  // namespace bro::engine
