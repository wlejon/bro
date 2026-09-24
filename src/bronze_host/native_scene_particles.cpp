// native_scene_particles.cpp — SceneGraph.createParticles: the 2D ParticleNode
// and its emitter options (docs/scene-api.js ParticleNodeOptions). The wrapper
// hands the options over as JSON, as createParticles3D does.

#include "bronze_host/native_scene_internal.h"
#include "natives/scene/native_scene_decl.h"
#include "scene/particle_node.h"
#include "util/log.h"
#include <json.hpp>

#include <string>

namespace bro::bronze_host {
namespace {

float num(const nlohmann::json& o, const char* key, float def) {
    auto it = o.find(key);
    return (it != o.end() && it->is_number()) ? it->get<float>() : def;
}

// "#rgb" / "#rrggbbaa" / "rgba(...)" / any CSS colour, or [r, g, b(, a)] in 0..1.
bool colorOf(const nlohmann::json& v, bromath::Color& out) {
    if (v.is_string()) {
        float r, g, b, a;
        if (!parseHexOrCssColor(v.get<std::string>(), r, g, b, a)) return false;
        out = {r, g, b, a};
        return true;
    }
    if (v.is_array() && v.size() >= 3 && v[0].is_number() && v[1].is_number() && v[2].is_number()) {
        out = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>(),
               v.size() >= 4 && v[3].is_number() ? v[3].get<float>() : 1.0f};
        return true;
    }
    return false;
}

// The emitter half of the options, as the QuickJS binding's applyParticleOpts
// read them. maxParticles runs first (it reallocates the pool) and burst last.
void applyParticleOpts(scene::ParticleNode* node, const nlohmann::json& j) {
    if (auto it = j.find("maxParticles"); it != j.end() && it->is_number())
        node->setMaxParticles(it->get<int>());
    if (auto it = j.find("texture"); it != j.end() && it->is_string())
        node->setTexturePath(it->get<std::string>());
    if (auto it = j.find("blend"); it != j.end() && it->is_string())
        node->setBlend(it->get<std::string>() == "additive" ? scene::ParticleNode::Blend::Additive
                                                            : scene::ParticleNode::Blend::Normal);
    if (auto it = j.find("rate"); it != j.end() && it->is_number())
        node->setRate(it->get<float>());
    if (auto it = j.find("lifetime"); it != j.end()) {
        if (it->is_number()) node->setLifetime(it->get<float>(), it->get<float>());
        else if (it->is_object()) node->setLifetime(num(*it, "min", 0.5f), num(*it, "max", 1.0f));
    }
    if (auto it = j.find("velocity"); it != j.end() && it->is_object())
        node->setVelocity(num(*it, "angle", -90.0f), num(*it, "angleSpread", 360.0f),
                          num(*it, "speed", 100.0f), num(*it, "speedSpread", 0.0f));
    if (auto it = j.find("gravity"); it != j.end()) {
        if (it->is_array() && it->size() >= 2 && (*it)[0].is_number() && (*it)[1].is_number())
            node->setGravity((*it)[0].get<float>(), (*it)[1].get<float>());
        else if (it->is_object())
            node->setGravity(num(*it, "x", 0.0f), num(*it, "y", 0.0f));
    }
    if (auto it = j.find("size"); it != j.end()) {
        if (it->is_number()) node->setSize(it->get<float>(), it->get<float>());
        else if (it->is_object()) node->setSize(num(*it, "start", 6.0f), num(*it, "end", 0.0f));
    }
    if (auto it = j.find("color"); it != j.end()) {
        bromath::Color start{1, 1, 1, 1};
        if (it->is_object()) {
            auto s = it->find("start");
            if (s != it->end()) colorOf(*s, start);
            bromath::Color end{start.r, start.g, start.b, 0.0f};   // fades out unless told otherwise
            auto e = it->find("end");
            if (e != it->end()) colorOf(*e, end);
            node->setColors(start, end);
        } else if (colorOf(*it, start)) {
            node->setColors(start, bromath::Color{start.r, start.g, start.b, 0.0f});
        }
    }
    if (auto it = j.find("rotation"); it != j.end() && it->is_object())
        node->setRotation(num(*it, "start", 0.0f), num(*it, "spinSpeed", 0.0f), num(*it, "spinSpread", 0.0f));
    if (auto it = j.find("drag"); it != j.end() && it->is_number())
        node->setDrag(it->get<float>());

    auto ap = j.find("autoplay");
    if (ap != j.end() && ap->is_boolean() && !ap->get<bool>()) node->stop();
    else node->play();

    if (auto it = j.find("burst"); it != j.end() && it->is_number())
        node->burst(it->get<int>());
}

}  // namespace
}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

// Transform keys: `position` [x, y(, z)] or `x` / `y`; plus `name`, `visible`.
void* bro_scene_SceneGraph_createParticles(void* self, const char* jsonOpts) {
    auto* g = graphOf(self);
    if (!g) return nullptr;
    auto* node = g->createParticles();
    g->root()->addChild(node);
    if (jsonOpts && jsonOpts[0]) {
        try {
            const auto j = nlohmann::json::parse(jsonOpts);
            if (j.is_object()) {
                if (auto it = j.find("name"); it != j.end() && it->is_string()) node->setName(it->get<std::string>());
                auto pos = j.find("position");
                if (pos != j.end() && pos->is_array() && pos->size() >= 2) {
                    const float z = pos->size() >= 3 && (*pos)[2].is_number() ? (*pos)[2].get<float>() : 0.0f;
                    node->setPosition((*pos)[0].get<float>(), (*pos)[1].get<float>(), z);
                } else if (j.contains("x") || j.contains("y")) {
                    node->setPosition(num(j, "x", 0.0f), num(j, "y", 0.0f), 0.0f);
                }
                if (auto it = j.find("visible"); it != j.end() && it->is_boolean()) node->setVisible(it->get<bool>());
                applyParticleOpts(node, j);
            }
        } catch (const std::exception& e) {
            LOG_WARN("SceneGraph.createParticles: failed to parse options: %s", e.what());
        }
    } else {
        node->play();
    }
    return wrapNode(node, g);
}

}  // extern "C"
