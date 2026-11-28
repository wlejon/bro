// What setShaderModule and setShaderAsync are given, read for both, and
// setShaderAsync's half: every stage of the hook compiled on threads of their
// own while the page runs, its callback called from a frame once all are in
// (js/scene.js then sets the module, which takes them built).
#include "bronze_host/native_scene_internal.h"
#include "natives/scene/native_scene_decl.h"
#include "bronze_host/host_runtime.h"
#include "engine/engine.h"
#include "util/page_pump.h"

#include <json.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace bro::bronze_host {

using json = nlohmann::json;

bool readShaderModuleArgs(scene::SceneNode* n, const char* module, const char* file, const char* modulesJson,
                          ShaderModuleArgs& out, std::string& err) {
    using Target = scene::SceneRenderer::CustomShaderTarget;
    out.target = Target::Static;
    out.isSkinned = false;
    if (n->type() == scene::SceneNode::Type::Mesh) {
        auto* mn = static_cast<scene::MeshNode*>(n);
        if (mn->isVolume()) {
            out.target = Target::Volume;
        } else if (mn->asSkinnedMesh()) {
            out.target = Target::Skinned;
            out.isSkinned = true;
        }
    } else if (n->type() == scene::SceneNode::Type::InstancedMesh) {
        out.target = Target::Instanced;
    } else {
        err = "node is not a MeshNode or InstancedMeshNode";
        return false;
    }
    out.source = module ? module : "";
    out.file = file && *file ? file : "hook.bk";
    if (out.source.empty()) {
        err = "no module source";
        return false;
    }
    // The modules it imports beside the engine's: { name: source }, and its
    // knobs under __knobs__.
    out.modules.clear();
    out.knobs.clear();
    if (modulesJson && *modulesJson) {
        auto j = json::parse(modulesJson, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            err = "modules must map names to sources";
            return false;
        }
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key() == "__knobs__" && it.value().is_object()) {
                for (auto kit = it.value().begin(); kit != it.value().end(); ++kit) {
                    if (kit.value().is_number()) {
                        out.knobs.emplace_back(kit.key(), kit.value().get<double>());
                    } else if (kit.value().is_boolean()) {
                        out.knobs.emplace_back(kit.key(), kit.value().get<bool>() ? 1.0 : 0.0);
                    }
                }
                continue;
            }
            if (!it.value().is_string()) {
                err = "module '" + it.key() + "' is not a source string";
                return false;
            }
            out.modules.emplace_back(it.key(), it.value().get<std::string>());
        }
    }
    return true;
}

namespace {

// A setShaderAsync waiting for its stages (page thread only).
struct PendingShader {
    std::vector<std::function<bool()>> ready;
    std::unique_ptr<ev::Persistent> callback;
    ev::Realm* realm = nullptr;
    bool allReady() const {
        for (const auto& r : ready) {
            if (!r()) return false;
        }
        return true;
    }
};

std::vector<std::unique_ptr<PendingShader>>& pendingShaders() {
    static std::vector<std::unique_ptr<PendingShader>> s;
    return s;
}

// Calls the callback of each one whose stages are in; headless waits for
// all of them (pumping), so a test's promise settles in the frame after it
// was asked for.
void settlePendingShaders(bool waitAll) {
    auto& pending = pendingShaders();
    if (pending.empty()) return;
    std::vector<std::unique_ptr<PendingShader>> due;
    for (auto it = pending.begin(); it != pending.end();) {
        if (waitAll) {
            const PendingShader& p = **it;
            util::waitPumpingUntil([&p](int ms) {
                const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
                while (!p.allReady()) {
                    if (ms >= 0 && std::chrono::steady_clock::now() >= end) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return true;
            });
        }
        if ((*it)->allReady()) {
            due.push_back(std::move(*it));
            it = pending.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& p : due) {
        ev::RealmScope scope(p->realm);
        const ev::Value fn = p->callback->get();
        if (ev::isFunction(fn)) ev::call(fn, ev::undefined(), {});
    }
    if (!due.empty() && ev::microtasksPending()) ev::drainMicrotasks();
}

void dropRealm(ev::Realm* realm) {
    auto& pending = pendingShaders();
    std::erase_if(pending, [realm](const std::unique_ptr<PendingShader>& p) { return p->realm == realm; });
}

void installSettle() {
    static engine::Engine* s_engine = nullptr;
    engine::Engine* eng = hostEngine();
    if (!eng || s_engine == eng) return;
    s_engine = eng;
    static bool s_realmHook = false;
    if (!s_realmHook) {
        s_realmHook = true;
        ev::addRealmDestroyHook(dropRealm);
    }
    const bool waitAll = eng->displayMode() != engine::DisplayMode::Windowed;
    eng->addFramePump([waitAll] { settlePendingShaders(waitAll); });
    eng->addShutdownHook([] {
        pendingShaders().clear();
        s_engine = nullptr;
    });
}

}  // namespace

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

static thread_local std::string tl_prepareErr;

const char* bro_scene_SceneNode_prepareShaderModule(void* self, const char* module, const char* file,
                                                    const char* modulesJson, uint64_t callback) {
    auto* n = nodeOf(self);
    auto* cell = nodeCellOf(self);
    if (!n || !cell || !cell->graph()) {
        tl_prepareErr = "setShaderAsync: node or scene destroyed";
        return tl_prepareErr.c_str();
    }
    ShaderModuleArgs args;
    std::string err;
    if (!readShaderModuleArgs(n, module, file, modulesJson, args, err)) {
        tl_prepareErr = "setShaderAsync: " + err;
        return tl_prepareErr.c_str();
    }
    installSettle();
    auto p = std::make_unique<PendingShader>();
    using Target = scene::SceneRenderer::CustomShaderTarget;
    p->ready.push_back(
        scene::SceneRenderer::prepareShaderModule(args.target, args.source, args.file, args.modules, args.knobs));
    // A skinned mesh draws its static pose too (setShaderModule compiles both).
    if (args.isSkinned) {
        p->ready.push_back(
            scene::SceneRenderer::prepareShaderModule(Target::Static, args.source, args.file, args.modules, args.knobs));
    }
    p->callback = std::make_unique<ev::Persistent>(ev::fromBits(callback));
    p->realm = ev::currentRealm();
    pendingShaders().push_back(std::move(p));
    return "";
}

}  // extern "C"
