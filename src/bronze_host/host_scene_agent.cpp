#if BRO_WITH_3D

#include "bronze_host/native_scene_internal.h"
#include "host_ai_internal.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/agent_binding.h"
#include "physics/physics_world.h"

#include <brogameagent/api.h>
#include <brogameagent/brogameagent.h>
#include <brogameagent/capability.h>
#include <brogameagent/policy.h>

#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

using namespace brogameagent::api;

// ---------------------------------------------------------------------------
// Helpers: Builtin & Custom Capabilities, Lane Waypoints
//
// JS-authored capabilities live in brogameagent's registry
// (bro.ai.game.registerCapability); a name from opts.capabilities that is not
// a built-in becomes brogameagent::api::makeRegisteredCapability(name), whose
// gate/start/advance/cancel call the registered spec's functions.
// ---------------------------------------------------------------------------

static bool addBuiltinByName(brogameagent::CapabilitySet& set, const std::string& name) {
    using namespace brogameagent;
    if (name == "move_to")           set.add(makeMoveToCapability());
    else if (name == "lane_walk")    set.add(makeLaneWalkCapability());
    else if (name == "basic_attack") set.add(makeBasicAttackCapability());
    else if (name == "cast_ability") set.add(makeCastAbilityCapability());
    else if (name == "flee")         set.add(makeFleeCapability());
    else if (name == "hold")         set.add(makeHoldCapability());
    else return false;
    return true;
}

// A name that is neither a built-in nor registered on this thread is an
// error: silently dropping it leaves an agent that never does what the
// caller listed.
static bool parseCapabilitiesList(Value arr, brogameagent::CapabilitySet& set, std::string& unknown) {
    if (!ev::isObject(arr)) return true;
    ev::Persistent root(arr);
    Value lenVal = ev::getProperty(root.get(), "length");
    if (!ev::isNumber(lenVal)) return true;
    uint32_t len = static_cast<uint32_t>(ev::toDouble(lenVal));
    for (uint32_t i = 0; i < len; ++i) {
        Value v = ev::getElement(root.get(), i);
        if (!ev::isString(v)) continue;
        std::string name = ev::toUtf8(v);
        if (addBuiltinByName(set, name)) continue;
        if (auto cap = makeRegisteredCapability(name)) {
            set.add(std::move(cap));
            continue;
        }
        unknown = std::move(name);
        return false;
    }
    return true;
}

static void parseLaneWaypoints(Value arr, brogameagent::CapabilitySet& set) {
    if (!ev::isObject(arr)) return;
    ev::Persistent root(arr);
    Value lenVal = ev::getProperty(root.get(), "length");
    if (!ev::isNumber(lenVal)) return;
    uint32_t len = static_cast<uint32_t>(ev::toDouble(lenVal));
    std::vector<bromath::Vec2> wps;
    wps.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        Value pt = ev::getElement(root.get(), i);
        if (ev::isObject(pt)) {
            ev::Persistent p(pt);
            Value vx = ev::getProperty(p.get(), "x");
            Value vz = ev::getProperty(p.get(), "z");
            double x = ev::isNumber(vx) ? ev::toDouble(vx) : 0.0;
            double z = ev::isNumber(vz) ? ev::toDouble(vz) : 0.0;
            wps.push_back({static_cast<float>(x), static_cast<float>(z)});
        }
    }
    set.setLaneWaypoints(std::move(wps));
}

// ---------------------------------------------------------------------------
// Self Proxy for Think Hook
// ---------------------------------------------------------------------------

inline constexpr uint32_t kHostSelfProxyTag = 0x53454C46u;  // 'SELF'

struct HostSelfProxyCell {
    uint32_t tag = kHostSelfProxyTag;
    scene::AgentBinding* binding = nullptr;
};

inline HostSelfProxyCell* selfProxyCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* h = static_cast<HostSelfProxyCell*>(ev::handleData(v));
    return (h && h->tag == kHostSelfProxyTag) ? h : nullptr;
}

static HostClass g_selfProxyClass;

static bool extractXZ(Value val, float& x, float& z) {
    if (!ev::isObject(val)) return false;
    if (auto* ag = unwrapAgent(val)) {
        x = ag->agent.x();
        z = ag->agent.z();
        return true;
    }
    if (auto* sp = selfProxyCellOf(val)) {
        if (sp->binding && sp->binding->agent()) {
            x = sp->binding->agent()->x();
            z = sp->binding->agent()->z();
            return true;
        }
    }
    ev::Persistent p(val);
    Value vx = ev::getProperty(p.get(), "x");
    Value vz = ev::getProperty(p.get(), "z");
    if (ev::isNumber(vx) && ev::isNumber(vz)) {
        x = static_cast<float>(ev::toDouble(vx));
        z = static_cast<float>(ev::toDouble(vz));
        return true;
    }
    return false;
}

static void ensureSelfProxyClassInstalled() {
    static bool s_installed = false;
    if (s_installed) return;
    s_installed = true;

    g_selfProxyClass.install("AgentSelfProxy", 0, nullptr, [](ObjectBuilder& b) {
        b.accessor("hp", [](Value self_, std::span<const Value>) -> Value {
            auto* sp = selfProxyCellOf(self_);
            return ev::fromDouble((sp && sp->binding && sp->binding->agent()) ? sp->binding->agent()->unit().hp : 0.0);
        }, nullptr);

        b.accessor("mana", [](Value self_, std::span<const Value>) -> Value {
            auto* sp = selfProxyCellOf(self_);
            return ev::fromDouble((sp && sp->binding && sp->binding->agent()) ? sp->binding->agent()->unit().mana : 0.0);
        }, nullptr);

        b.accessor("x", [](Value self_, std::span<const Value>) -> Value {
            auto* sp = selfProxyCellOf(self_);
            return ev::fromDouble((sp && sp->binding && sp->binding->agent()) ? sp->binding->agent()->x() : 0.0);
        }, nullptr);

        b.accessor("z", [](Value self_, std::span<const Value>) -> Value {
            auto* sp = selfProxyCellOf(self_);
            return ev::fromDouble((sp && sp->binding && sp->binding->agent()) ? sp->binding->agent()->z() : 0.0);
        }, nullptr);

        b.accessor("id", [](Value self_, std::span<const Value>) -> Value {
            auto* sp = selfProxyCellOf(self_);
            return ev::fromDouble((sp && sp->binding && sp->binding->agent()) ? sp->binding->agent()->unit().id : 0.0);
        }, nullptr);

        b.accessor("teamId", [](Value self_, std::span<const Value>) -> Value {
            auto* sp = selfProxyCellOf(self_);
            return ev::fromDouble((sp && sp->binding && sp->binding->agent()) ? sp->binding->agent()->unit().teamId : 0.0);
        }, nullptr);

        b.accessor("attackRange", [](Value self_, std::span<const Value>) -> Value {
            auto* sp = selfProxyCellOf(self_);
            return ev::fromDouble((sp && sp->binding && sp->binding->agent()) ? sp->binding->agent()->unit().attackRange : 0.0);
        }, nullptr);

        b.accessor("alive", [](Value self_, std::span<const Value>) -> Value {
            auto* sp = selfProxyCellOf(self_);
            return ev::fromBool((sp && sp->binding && sp->binding->agent()) ? sp->binding->agent()->unit().alive() : false);
        }, nullptr);

        b.def("hold", 1, [](Value self_, std::span<const Value> a) -> Value {
            auto* sp = selfProxyCellOf(self_);
            if (!sp || !sp->binding) return ev::undefined();
            auto& act = sp->binding->pending();
            act = brogameagent::Action{};
            act.capId = brogameagent::kCapHold;
            if (!a.empty() && ev::isNumber(a[0])) act.dur = static_cast<float>(ev::toDouble(a[0]));
            return ev::undefined();
        });

        b.def("moveTo", 2, [](Value self_, std::span<const Value> a) -> Value {
            auto* sp = selfProxyCellOf(self_);
            if (!sp || !sp->binding || a.size() < 2) return ev::undefined();
            auto& act = sp->binding->pending();
            act = brogameagent::Action{};
            act.capId = brogameagent::kCapMoveTo;
            act.fx = static_cast<float>(ev::toDouble(a[0]));
            act.fz = static_cast<float>(ev::toDouble(a[1]));
            return ev::undefined();
        });

        b.def("attack", 1, [](Value self_, std::span<const Value> a) -> Value {
            auto* sp = selfProxyCellOf(self_);
            if (!sp || !sp->binding || a.empty()) return ev::undefined();
            auto& act = sp->binding->pending();
            act = brogameagent::Action{};
            act.capId = brogameagent::kCapBasicAttack;
            act.i0 = static_cast<int32_t>(ev::toDouble(a[0]));
            return ev::undefined();
        });

        b.def("cast", 2, [](Value self_, std::span<const Value> a) -> Value {
            auto* sp = selfProxyCellOf(self_);
            if (!sp || !sp->binding || a.empty()) return ev::undefined();
            auto& act = sp->binding->pending();
            act = brogameagent::Action{};
            act.capId = brogameagent::kCapCastAbility;
            act.i0 = static_cast<int32_t>(ev::toDouble(a[0]));
            if (a.size() >= 2) act.i1 = static_cast<int32_t>(ev::toDouble(a[1]));
            return ev::undefined();
        });

        b.def("flee", 2, [](Value self_, std::span<const Value> a) -> Value {
            auto* sp = selfProxyCellOf(self_);
            if (!sp || !sp->binding) return ev::undefined();
            auto& act = sp->binding->pending();
            act = brogameagent::Action{};
            act.capId = brogameagent::kCapFlee;
            if (a.size() >= 2) {
                act.fx = static_cast<float>(ev::toDouble(a[0]));
                act.fz = static_cast<float>(ev::toDouble(a[1]));
            }
            return ev::undefined();
        });

        b.def("laneWalk", 0, [](Value self_, std::span<const Value>) -> Value {
            auto* sp = selfProxyCellOf(self_);
            if (!sp || !sp->binding) return ev::undefined();
            auto& act = sp->binding->pending();
            act = brogameagent::Action{};
            act.capId = brogameagent::kCapLaneWalk;
            return ev::undefined();
        });

        b.def("useCapability", 1, [](Value self_, std::span<const Value> a) -> Value {
            auto* sp = selfProxyCellOf(self_);
            if (!sp || !sp->binding || a.empty()) return ev::undefined();
            if (!ev::isString(a[0])) return ev::throwTypeError("useCapability(name, arg0?, arg1?): name must be a string");
            const int id = registeredCapabilityId(ev::toUtf8(a[0]));
            if (id < 0) {
                return ev::throwTypeError("useCapability: \"" + ev::toUtf8(a[0]) +
                                          "\" is not registered with bro.ai.game.registerCapability");
            }
            auto& act = sp->binding->pending();
            act = brogameagent::Action{};
            act.capId = id;
            if (a.size() >= 2 && ev::isNumber(a[1])) act.i0 = static_cast<int32_t>(ev::toDouble(a[1]));
            if (a.size() >= 3 && ev::isNumber(a[2])) act.i1 = static_cast<int32_t>(ev::toDouble(a[2]));
            return ev::undefined();
        });

        b.def("distanceTo", 1, [](Value self_, std::span<const Value> a) -> Value {
            auto* sp = selfProxyCellOf(self_);
            if (!sp || !sp->binding || !sp->binding->agent() || a.empty()) return ev::fromDouble(0.0);
            float ox = 0.0f, oz = 0.0f;
            if (!extractXZ(a[0], ox, oz)) return ev::fromDouble(0.0);
            float dx = sp->binding->agent()->x() - ox;
            float dz = sp->binding->agent()->z() - oz;
            return ev::fromDouble(std::sqrt(dx * dx + dz * dz));
        });

        b.def("inRange", 2, [](Value self_, std::span<const Value> a) -> Value {
            auto* sp = selfProxyCellOf(self_);
            if (!sp || !sp->binding || !sp->binding->agent() || a.empty()) return ev::fromBool(false);
            float ox = 0.0f, oz = 0.0f;
            if (!extractXZ(a[0], ox, oz)) return ev::fromBool(false);
            float dx = sp->binding->agent()->x() - ox;
            float dz = sp->binding->agent()->z() - oz;
            float range = sp->binding->agent()->unit().attackRange;
            if (a.size() >= 2 && ev::isNumber(a[1])) range = static_cast<float>(ev::toDouble(a[1]));
            return ev::fromBool((dx * dx + dz * dz) <= range * range);
        });
    });
}

static Value makeSelfProxy(scene::AgentBinding* b, Value agentRef) {
    ensureSelfProxyClassInstalled();
    auto* cell = new HostSelfProxyCell();
    cell->binding = b;
    Value inst = g_selfProxyClass.make(cell, [](void* p) {
        delete static_cast<HostSelfProxyCell*>(p);
    });
    ev::Persistent p(inst);
    p.set(ev::setProperty(p.get(), "agent", agentRef));
    return p.get();
}

// ---------------------------------------------------------------------------
// BronzeThinkHook
// ---------------------------------------------------------------------------

class BronzeThinkHook : public scene::ThinkHook {
public:
    BronzeThinkHook(Value thinkFn, Value worldRef, Value agentRef)
        : thinkFn_(thinkFn), worldRef_(worldRef), agentRef_(agentRef) {}

    void setBinding(scene::AgentBinding* b) { binding_ = b; }

    void think(const brogameagent::CapContext&,
               const brogameagent::CapabilitySet&,
               brogameagent::Action& out) override {
        if (!binding_ || !binding_->agent()) return;
        Value selfObj = makeSelfProxy(binding_, agentRef_.get());
        Value args[2] = { selfObj, worldRef_.get() };
        ev::call(thinkFn_.get(), ev::undefined(), args);
        if (binding_->pending().capId != brogameagent::kCapNone) {
            out = binding_->pending();
        }
    }

private:
    scene::AgentBinding* binding_ = nullptr;
    ev::Persistent thinkFn_;
    ev::Persistent worldRef_;
    ev::Persistent agentRef_;
};

// ---------------------------------------------------------------------------
// SceneGraph Agent methods
// ---------------------------------------------------------------------------

void installSceneGraphAgent(ObjectBuilder& b) {
    b.def("attachAIWorld", 1, [](Value self_, std::span<const Value> a) -> Value {
        auto* g = sceneGraphOf(self_);
        if (!g || a.empty()) return ev::undefined();
        auto* hw = unwrapWorld(a[0]);
        if (!hw) return ev::throwTypeError("attachAIWorld: expected a bro.ai.game.createWorld()");
        float stepHz = 60.0f;
        int maxSteps = 8;
        if (a.size() >= 2 && ev::isObject(a[1])) {
            ev::Persistent opts(a[1]);
            Value hzVal = ev::getProperty(opts.get(), "stepHz");
            if (ev::isNumber(hzVal)) stepHz = static_cast<float>(ev::toDouble(hzVal));
            Value maxVal = ev::getProperty(opts.get(), "maxStepsPerFrame");
            if (ev::isNumber(maxVal)) maxSteps = static_cast<int>(ev::toDouble(maxVal));
        }
        auto keepAlive = std::make_shared<ev::Persistent>(a[0]);
        g->attachAIWorld(&hw->world, stepHz, maxSteps, std::move(keepAlive));
        return ev::undefined();
    });

    b.def("detachAIWorld", 0, [](Value self_, std::span<const Value>) -> Value {
        auto* g = sceneGraphOf(self_);
        if (g) g->detachAIWorld();
        return ev::undefined();
    });
}

// ---------------------------------------------------------------------------
// SceneNode Agent methods
// ---------------------------------------------------------------------------

void installSceneNodeAgent(ObjectBuilder& b) {
    b.def("attachAgent", 2, [](Value self_, std::span<const Value> a) -> Value {
        auto* cell = sceneNodeCellOf(self_);
        auto* node = cell ? cell->node() : nullptr;
        auto* g = cell ? cell->graph() : nullptr;
        if (!node || !g || a.size() < 2) {
            return ev::throwTypeError("node.attachAgent(world, agent, opts)");
        }
        auto* hw = unwrapWorld(a[0]);
        if (!hw) return ev::throwTypeError("attachAgent: first arg must be a bro.ai.game.createWorld()");
        auto* hAgent = unwrapAgent(a[1]);
        if (!hAgent) return ev::throwTypeError("attachAgent: second arg must be a bro.ai.game.createAgent()");

        auto* binding = g->attachAgentBinding(node);
        if (!binding) return ev::throwTypeError("attachAgent: failed to create binding");
        binding->setAgent(&hAgent->agent);

        binding->setThinkHz(15.0f);
        binding->setYOffset(0.0f);
        binding->setFaceMovement(true);
        binding->setGroundFollow({});
        binding->stopNavigation();
        binding->setNavMesh(nullptr);
        binding->clearKeepAlives();

        // Pin the world and agent objects
        binding->addKeepAlive(std::make_shared<ev::Persistent>(a[0]));
        binding->addKeepAlive(std::make_shared<ev::Persistent>(a[1]));

        binding->capabilities() = brogameagent::CapabilitySet{};
        bool explicitList = false;

        if (a.size() >= 3 && ev::isObject(a[2])) {
            ev::Persistent opts(a[2]);

            Value capsVal = ev::getProperty(opts.get(), "capabilities");
            if (ev::isObject(capsVal)) {
                explicitList = true;
                std::string unknown;
                if (!parseCapabilitiesList(capsVal, binding->capabilities(), unknown)) {
                    g->detachAgentBinding(node);
                    return ev::throwTypeError("attachAgent: unknown capability \"" + unknown +
                                              "\" (not a built-in, and not registered with "
                                              "bro.ai.game.registerCapability on this thread)");
                }
            }

            Value hzVal = ev::getProperty(opts.get(), "thinkHz");
            if (ev::isNumber(hzVal)) binding->setThinkHz(static_cast<float>(ev::toDouble(hzVal)));

            Value yVal = ev::getProperty(opts.get(), "yOffset");
            if (ev::isNumber(yVal)) binding->setYOffset(static_cast<float>(ev::toDouble(yVal)));

            Value fmVal = ev::getProperty(opts.get(), "faceMovement");
            if (ev::isBool(fmVal)) binding->setFaceMovement(ev::toBool(fmVal));

            Value wpsVal = ev::getProperty(opts.get(), "laneWaypoints");
            if (ev::isObject(wpsVal)) parseLaneWaypoints(wpsVal, binding->capabilities());

            Value avoidVal = ev::getProperty(opts.get(), "avoidance");
            if (!ev::isUndefined(avoidVal) && !ev::isNull(avoidVal)) {
                applyAgentAvoidance(avoidVal, hAgent->agent);
            }

            Value nmVal = ev::getProperty(opts.get(), "navMesh");
            if (!ev::isUndefined(nmVal) && !ev::isNull(nmVal)) {
                auto* hMesh = unwrapNavMesh(nmVal);
                if (!hMesh || !hMesh->mesh) {
                    return ev::throwTypeError("attachAgent: navMesh must be a bro.ai.game.bakeNavMesh()/loadNavMesh() object");
                }
                binding->setNavMesh(hMesh->mesh);
            }

            Value polVal = ev::getProperty(opts.get(), "policy");
            if (ev::isString(polVal)) {
                std::string pol = ev::toUtf8(polVal);
                if (pol == "scripted_minion") {
                    binding->setPolicy(brogameagent::makeScriptedMinionPolicy());
                }
            }

            Value thinkVal = ev::getProperty(opts.get(), "think");
            if (ev::isFunction(thinkVal)) {
                auto hook = std::make_unique<BronzeThinkHook>(thinkVal, a[0], a[1]);
                hook->setBinding(binding);
                binding->setThinkHook(std::move(hook));
            }

            Value gfVal = ev::getProperty(opts.get(), "groundFollow");
            if (ev::isObject(gfVal)) {
                ev::Persistent gf(gfVal);
                Value modeVal = ev::getProperty(gf.get(), "mode");
                std::string mode = ev::isString(modeVal) ? ev::toUtf8(modeVal) : "";
                float rayStart = 100.0f;
                float rayLength = 200.0f;
                Value rsVal = ev::getProperty(gf.get(), "rayStart");
                if (ev::isNumber(rsVal)) rayStart = static_cast<float>(ev::toDouble(rsVal));
                Value rlVal = ev::getProperty(gf.get(), "rayLength");
                if (ev::isNumber(rlVal)) rayLength = static_cast<float>(ev::toDouble(rlVal));

                if (mode == "terrain") {
                    Value tv = ev::getProperty(gf.get(), "terrain");
                    auto* tc = terrainCellOf(tv);
                    if (!tc) {
                        return ev::throwTypeError("attachAgent: groundFollow mode 'terrain' requires a scene.createTerrain() object");
                    }
                    binding->addKeepAlive(std::make_shared<ev::Persistent>(tv));
                    binding->setGroundFollow([tc, rayStart, rayLength](float x, float z, float& outY) {
                        return terrainSampleHeight(tc, x, z, rayStart, rayLength, outY);
                    });
                } else if (mode == "raycast") {
#if BRO_WITH_PHYSICS
                    auto* pw = g->physicsWorld();
                    if (!pw) {
                        return ev::throwTypeError("attachAgent: groundFollow mode 'raycast' requires an active physics world");
                    }
                    physics::QueryFilter filter;
                    Value lv = ev::getProperty(gf.get(), "layers");
                    if (ev::isObject(lv)) {
                        ev::Persistent lvr(lv);
                        Value lenV = ev::getProperty(lvr.get(), "length");
                        if (ev::isNumber(lenV)) {
                            uint32_t mask = 0;
                            uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
                            for (uint32_t i = 0; i < n; ++i) {
                                Value el = ev::getElement(lvr.get(), i);
                                int32_t idx = -1;
                                if (ev::isString(el)) {
                                    idx = pw->layerIndex(ev::toUtf8(el));
                                } else if (ev::isNumber(el)) {
                                    idx = static_cast<int32_t>(ev::toDouble(el));
                                }
                                if (idx >= 0 && idx < 32) mask |= (1u << idx);
                            }
                            filter.layerMask = mask;
                        }
                    }
                    scene::SceneGraph* graph = g;
                    binding->setGroundFollow([graph, filter, rayStart, rayLength](float x, float z, float& outY) {
                        auto* world = graph->physicsWorld();
                        if (!world || !world->isIdle()) return false;
                        physics::RayHit hit;
                        if (!world->raycastClosest(JPH::RVec3(x, rayStart, z),
                                                   JPH::Vec3(0, -1, 0),
                                                   hit, rayLength, filter)) {
                            return false;
                        }
                        outY = hit.position.GetY();
                        return true;
                    });
#else
                    return ev::throwTypeError("attachAgent: groundFollow mode 'raycast' requires a physics-enabled build");
#endif
                } else {
                    return ev::throwTypeError("attachAgent: groundFollow.mode must be 'terrain' or 'raycast'");
                }
            }
        }

        if (!explicitList) {
            brogameagent::addAllBuiltinCapabilities(binding->capabilities());
        }

        return self_;
    });

    b.def("detachAgent", 0, [](Value self_, std::span<const Value>) -> Value {
        auto* cell = sceneNodeCellOf(self_);
        auto* node = cell ? cell->node() : nullptr;
        auto* g = cell ? cell->graph() : nullptr;
        if (g && node) g->detachAgentBinding(node);
        return self_;
    });

    b.def("navigateTo", 1, [](Value self_, std::span<const Value> a) -> Value {
        auto* cell = sceneNodeCellOf(self_);
        auto* node = cell ? cell->node() : nullptr;
        auto* g = cell ? cell->graph() : nullptr;
        if (!node || !g || a.empty()) {
            return ev::throwTypeError("node.navigateTo(target, opts?)");
        }
        auto* binding = g->agentBinding(node);
        if (!binding || !binding->agent()) {
            return ev::throwTypeError("navigateTo: no agent attached (call node.attachAgent first)");
        }

        bromath::Vec3 target{0, 0, 0};
        if (!ev::isObject(a[0])) return ev::throwTypeError("navigateTo: target must be {x,y,z} or [x,y,z]");
        {
            ev::Persistent tp(a[0]);
            Value vx = ev::getProperty(tp.get(), "x");
            if (ev::isNumber(vx)) {
                target.x = static_cast<float>(ev::toDouble(vx));
                Value vy = ev::getProperty(tp.get(), "y");
                target.y = ev::isNumber(vy) ? static_cast<float>(ev::toDouble(vy)) : 0.0f;
                Value vz = ev::getProperty(tp.get(), "z");
                target.z = ev::isNumber(vz) ? static_cast<float>(ev::toDouble(vz)) : 0.0f;
            } else {
                Value e0 = ev::getElement(tp.get(), 0);
                Value e1 = ev::getElement(tp.get(), 1);
                Value e2 = ev::getElement(tp.get(), 2);
                if (ev::isNumber(e0)) target.x = static_cast<float>(ev::toDouble(e0));
                if (ev::isNumber(e1)) target.y = static_cast<float>(ev::toDouble(e1));
                if (ev::isNumber(e2)) target.z = static_cast<float>(ev::toDouble(e2));
            }
        }

        bromath::Vec3 extents = brogameagent::NavMesh::kDefaultExtents;
        float repathInterval = 0.0f;
        bool requireFullPath = false;

        if (a.size() >= 2 && ev::isObject(a[1])) {
            ev::Persistent opts(a[1]);
            Value mv = ev::getProperty(opts.get(), "navMesh");
            if (!ev::isUndefined(mv) && !ev::isNull(mv)) {
                auto* hMesh = unwrapNavMesh(mv);
                if (!hMesh || !hMesh->mesh) {
                    return ev::throwTypeError("navigateTo: navMesh must be a bro.ai.game.bakeNavMesh()/loadNavMesh() object");
                }
                binding->setNavMesh(hMesh->mesh);
            }

            Value ev_ = ev::getProperty(opts.get(), "extents");
            if (ev::isObject(ev_)) {
                ev::Persistent ep(ev_);
                Value ex = ev::getProperty(ep.get(), "x");
                Value ey = ev::getProperty(ep.get(), "y");
                Value ez = ev::getProperty(ep.get(), "z");
                if (ev::isNumber(ex)) extents.x = static_cast<float>(ev::toDouble(ex));
                if (ev::isNumber(ey)) extents.y = static_cast<float>(ev::toDouble(ey));
                if (ev::isNumber(ez)) extents.z = static_cast<float>(ev::toDouble(ez));
            }

            Value rpiVal = ev::getProperty(opts.get(), "repathInterval");
            if (ev::isNumber(rpiVal)) repathInterval = static_cast<float>(ev::toDouble(rpiVal));

            Value rfpVal = ev::getProperty(opts.get(), "requireFullPath");
            if (ev::isBool(rfpVal)) requireFullPath = ev::toBool(rfpVal);
        }

        if (!binding->navMesh()) {
            return ev::throwTypeError("navigateTo: no navMesh bound (pass one in attachAgent or navigateTo opts)");
        }

        return ev::fromBool(binding->navigateTo(target, extents, repathInterval, requireFullPath));
    });

    b.def("stopNavigation", 0, [](Value self_, std::span<const Value>) -> Value {
        auto* cell = sceneNodeCellOf(self_);
        auto* node = cell ? cell->node() : nullptr;
        auto* g = cell ? cell->graph() : nullptr;
        if (node && g) {
            if (auto* binding = g->agentBinding(node)) binding->stopNavigation();
        }
        return ev::undefined();
    });

    b.def("navigationInfo", 0, [](Value self_, std::span<const Value>) -> Value {
        auto* cell = sceneNodeCellOf(self_);
        auto* node = cell ? cell->node() : nullptr;
        auto* g = cell ? cell->graph() : nullptr;
        scene::AgentBinding* binding = (node && g) ? g->agentBinding(node) : nullptr;

        ObjectBuilder res;
        res.set("active", ev::fromBool(binding && binding->navigating()));
        res.set("partial", ev::fromBool(binding && binding->navPartial()));
        res.set("onLink", ev::fromBool(binding && binding->navOnLink()));
        return res.get();
    });
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
