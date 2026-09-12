#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "bronze_host/host_rigging_internal.h"
#include "scene/scene_graph.h"
#include "scene/scene_node.h"
#include "scene/sprite_node.h"
#include "scene/particle_node.h"
#include "scene/particles3d_node.h"
#include "scene/mesh_node.h"
#include "scene/skinned_mesh_node.h"
#include "scene/animation_player.h"

#include <cmath>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

static bool readBoneMask(Value v, std::vector<uint8_t>& out) {
    ev::TypedArrayInfo info = ev::typedArrayInfo(v);
    if (info && info.data) {
        const uint8_t* raw = reinterpret_cast<const uint8_t*>(info.data);
        out.assign(raw, raw + info.byteLength);
        return true;
    }
    if (!ev::isObject(v)) return false;
    Value lenVal = ev::getProperty(v, "length");
    int len = ev::isNumber(lenVal) ? static_cast<int>(ev::toDouble(lenVal)) : 0;
    out.resize(static_cast<size_t>(len));
    for (int i = 0; i < len; ++i) {
        Value elem = ev::getElement(v, static_cast<uint32_t>(i));
        double d = ev::isNumber(elem) ? ev::toDouble(elem) : 0.0;
        out[static_cast<size_t>(i)] = (d != 0.0) ? 1 : 0;
    }
    return true;
}

static void readPlayOptions(Value obj, scene::AnimationPlayer::PlayOptions& opts) {
    Value loopVal = ev::getProperty(obj, "loop");
    if (!ev::isUndefined(loopVal)) opts.loop = ev::toBool(loopVal);
    Value spdVal = ev::getProperty(obj, "speed");
    if (ev::isNumber(spdVal)) opts.speed = static_cast<float>(ev::toDouble(spdVal));
    Value fadeVal = ev::getProperty(obj, "fadeTime");
    if (ev::isNumber(fadeVal)) opts.fadeTime = static_cast<float>(ev::toDouble(fadeVal));
    Value wtVal = ev::getProperty(obj, "weight");
    if (ev::isNumber(wtVal)) opts.weight = static_cast<float>(ev::toDouble(wtVal));
    Value maskVal = ev::getProperty(obj, "mask");
    if (!ev::isUndefined(maskVal) && !ev::isNull(maskVal)) {
        readBoneMask(maskVal, opts.mask);
    }
}

static Value addBlendSpaceImpl(Value self_, std::span<const Value> a, bool is2D) {
    const char* fn = is2D ? "addBlendSpace2D" : "addBlendSpace1D";
    auto* n = sceneNodeOf(self_);
    if (!n || n->type() != scene::SceneNode::Type::Mesh)
        return ev::throwTypeError(std::string(fn) + ": node is not a skinned mesh");
    auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
    if (!sm)
        return ev::throwTypeError(std::string(fn) + ": node is not a skinned mesh");
    if (a.size() < 2 || !ev::isString(a[0]) || !ev::isObject(a[1]))
        return ev::throwTypeError(std::string(fn) + "(name, points[])");

    std::vector<scene::AnimationPlayer::BlendSpacePoint> points;
    Value lenVal = ev::getProperty(a[1], "length");
    int len = ev::isNumber(lenVal) ? static_cast<int>(ev::toDouble(lenVal)) : 0;
    for (int i = 0; i < len; ++i) {
        Value e = ev::getElement(a[1], static_cast<uint32_t>(i));
        if (!ev::isObject(e))
            return ev::throwTypeError(std::string(fn) + ": points[" + std::to_string(i) + "] is not an object");

        scene::AnimationPlayer::BlendSpacePoint p;
        Value clipVal = ev::getProperty(e, "clip");
        p.clip = ev::isString(clipVal) ? ev::toUtf8(clipVal) : "";
        Value tsVal = ev::getProperty(e, "timescale");
        p.timescale = ev::isNumber(tsVal) ? static_cast<float>(ev::toDouble(tsVal)) : 1.0f;
        Value posVal = ev::getProperty(e, "pos");
        if (is2D) {
            bool ok = ev::isObject(posVal);
            if (ok) {
                Value e0 = ev::getElement(posVal, 0);
                Value e1 = ev::getElement(posVal, 1);
                if (ev::isNumber(e0) && ev::isNumber(e1)) {
                    p.pos[0] = static_cast<float>(ev::toDouble(e0));
                    p.pos[1] = static_cast<float>(ev::toDouble(e1));
                } else {
                    ok = false;
                }
            }
            if (!ok)
                return ev::throwTypeError(std::string(fn) + ": points[" + std::to_string(i) + "].pos must be [x, y]");
        } else {
            if (!ev::isNumber(posVal))
                return ev::throwTypeError(std::string(fn) + ": points[" + std::to_string(i) + "].pos must be a number");
            p.pos[0] = static_cast<float>(ev::toDouble(posVal));
        }
        points.push_back(std::move(p));
    }

    std::string name = ev::toUtf8(a[0]);
    auto& player = sm->ensurePlayer();
    bool ok = is2D ? player.addBlendSpace2D(name, std::move(points))
                   : player.addBlendSpace1D(name, std::move(points));
    if (!ok) {
        return ev::throwTypeError(std::string(fn) + ": '" + name +
            "' needs at least one point and every clip registered via addClip first");
    }
    return self_;
}

} // namespace

void installSceneNodeAnim(ObjectBuilder& b) {
    b.def("setSkeleton", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh)
            return ev::throwTypeError("setSkeleton: node is not a skinned mesh");
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (!sm) return ev::throwTypeError("setSkeleton: node is not a skinned mesh");
        if (a.empty()) return ev::throwTypeError("setSkeleton: missing Skeleton");
        auto* sk = hostSkeletonOf(a[0]);
        if (!sk) return ev::throwTypeError("setSkeleton: argument must be a Skeleton");
        sm->ensurePlayer().setSkeleton(std::make_shared<bromesh::Skeleton>(*sk));
        return self_;
    });

    b.def("addClip", 2, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh)
            return ev::throwTypeError("addClip: node is not a skinned mesh");
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (!sm) return ev::throwTypeError("addClip: node is not a skinned mesh");
        if (a.size() < 2 || !ev::isString(a[0]))
            return ev::throwTypeError("addClip(name, animation)");
        auto* anim = hostAnimationOf(a[1]);
        if (!anim)
            return ev::throwTypeError("addClip: second argument must be an Animation");
        sm->ensurePlayer().addClip(ev::toUtf8(a[0]), std::make_shared<bromesh::Animation>(*anim));
        return self_;
    });

    b.def("getBoneWorldMatrix", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh)
            return ev::throwTypeError("getBoneWorldMatrix: node is not a skinned mesh");
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (!sm) return ev::throwTypeError("getBoneWorldMatrix: node is not a skinned mesh");
        auto* player = sm->player();
        if (!player || a.empty()) return ev::null();
        float m[16];
        bool ok = false;
        if (ev::isString(a[0])) {
            ok = player->boneWorldMatrix(ev::toUtf8(a[0]), m);
        } else if (ev::isNumber(a[0])) {
            ok = player->boneWorldMatrix(static_cast<int>(ev::toDouble(a[0])), m);
        }
        if (!ok) return ev::null();
        Value arr = ev::createTypedArray(bronze::embed::elements::Float32, 16);
        ev::fillTypedArray(arr, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(m), sizeof(m)));
        return arr;
    });

    b.def("play", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::undefined();
        if (n->type() == scene::SceneNode::Type::Sprite) {
            auto* s = static_cast<scene::SpriteNode*>(n);
            if (!a.empty() && ev::isString(a[0])) s->play(ev::toUtf8(a[0]));
            else s->resume();
        } else if (n->type() == scene::SceneNode::Type::Particles) {
            static_cast<scene::ParticleNode*>(n)->play();
        } else if (n->type() == scene::SceneNode::Type::Particles3D) {
            static_cast<scene::Particles3DNode*>(n)->play();
        } else if (n->type() == scene::SceneNode::Type::Mesh) {
            auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
            if (sm) {
                if (!a.empty() && ev::isString(a[0])) {
                    auto& player = sm->ensurePlayer();
                    scene::AnimationPlayer::PlayOptions opts;
                    if (a.size() > 1 && ev::isObject(a[1])) readPlayOptions(a[1], opts);
                    std::string name = ev::toUtf8(a[0]);
                    if (!player.play(name, opts)) {
                        return ev::throwTypeError("play: unknown clip or blend space '" + name + "' (addClip / addBlendSpace first) or no skeleton (setSkeleton first)");
                    }
                } else if (sm->player()) {
                    sm->player()->resume();
                }
            }
        }
        return self_;
    });

    b.def("stop", 0, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::undefined();
        if (n->type() == scene::SceneNode::Type::Sprite) {
            static_cast<scene::SpriteNode*>(n)->stop();
        } else if (n->type() == scene::SceneNode::Type::Particles) {
            static_cast<scene::ParticleNode*>(n)->stop();
        } else if (n->type() == scene::SceneNode::Type::Particles3D) {
            static_cast<scene::Particles3DNode*>(n)->stop();
        } else if (n->type() == scene::SceneNode::Type::Mesh) {
            auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
            if (sm && sm->player()) {
                float fade = 0.0f;
                if (!a.empty() && ev::isObject(a[0])) {
                    Value fVal = ev::getProperty(a[0], "fadeTime");
                    if (ev::isNumber(fVal)) fade = static_cast<float>(ev::toDouble(fVal));
                }
                sm->player()->stop(fade);
            }
        }
        return self_;
    });

    b.def("pause", 0, [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::undefined();
        if (n->type() == scene::SceneNode::Type::Sprite) {
            static_cast<scene::SpriteNode*>(n)->stop();
        } else if (n->type() == scene::SceneNode::Type::Mesh) {
            auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
            if (sm && sm->player()) sm->player()->pause();
        }
        return self_;
    });

    b.def("resume", 0, [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::undefined();
        if (n->type() == scene::SceneNode::Type::Sprite) {
            static_cast<scene::SpriteNode*>(n)->resume();
        } else if (n->type() == scene::SceneNode::Type::Mesh) {
            auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
            if (sm && sm->player()) sm->player()->resume();
        }
        return self_;
    });

    b.accessor("isPlaying", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::fromBool(false);
        if (n->type() == scene::SceneNode::Type::Sprite) {
            return ev::fromBool(static_cast<scene::SpriteNode*>(n)->isPlaying());
        } else if (n->type() == scene::SceneNode::Type::Particles) {
            return ev::fromBool(static_cast<scene::ParticleNode*>(n)->isPlaying());
        } else if (n->type() == scene::SceneNode::Type::Particles3D) {
            return ev::fromBool(static_cast<scene::Particles3DNode*>(n)->isPlaying());
        } else if (n->type() == scene::SceneNode::Type::Mesh) {
            auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
            if (sm && sm->player()) return ev::fromBool(sm->player()->isPlaying());
        }
        return ev::fromBool(false);
    }, nullptr);

    b.accessor("currentAnimation", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::undefined();
        if (n->type() == scene::SceneNode::Type::Sprite) {
            return ev::fromUtf8(static_cast<scene::SpriteNode*>(n)->currentAnimation());
        } else if (n->type() == scene::SceneNode::Type::Mesh) {
            auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
            if (sm && sm->player()) return ev::fromUtf8(sm->player()->currentClip());
        }
        return ev::undefined();
    }, nullptr);

    b.accessor("animationDuration", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (!n) return ev::undefined();
        if (n->type() == scene::SceneNode::Type::Mesh) {
            auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
            if (sm) return ev::fromDouble(sm->player() ? sm->player()->duration() : 0.0);
        }
        return ev::undefined();
    }, nullptr);

    b.accessor("animationTime",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (!n) return ev::undefined();
            if (n->type() == scene::SceneNode::Type::Mesh) {
                auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
                if (sm) return ev::fromDouble(sm->player() ? sm->player()->time() : 0.0);
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Mesh && !a.empty() && ev::isNumber(a[0])) {
                auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
                if (sm && sm->player()) {
                    sm->player()->setTime(static_cast<float>(ev::toDouble(a[0])));
                }
            }
            return ev::undefined();
        });

    b.accessor("animationSpeed",
        [](Value self_, std::span<const Value>) {
            auto* n = sceneNodeOf(self_);
            if (!n) return ev::undefined();
            if (n->type() == scene::SceneNode::Type::Mesh) {
                auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
                if (sm) return ev::fromDouble(sm->player() ? sm->player()->speed() : 1.0);
            }
            return ev::undefined();
        },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (n && n->type() == scene::SceneNode::Type::Mesh && !a.empty() && ev::isNumber(a[0])) {
                auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
                if (sm) sm->ensurePlayer().setSpeed(static_cast<float>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    b.accessor("onAnimationFinished",
        [](Value, std::span<const Value>) { return ev::undefined(); },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (!n || n->type() != scene::SceneNode::Type::Mesh) return ev::undefined();
            auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
            if (!sm) return ev::undefined();
            if (!a.empty() && ev::isFunction(a[0])) {
                auto fnRef = std::make_shared<ev::Persistent>(a[0]);
                sm->ensurePlayer().setOnFinished([fnRef](const std::string& name) {
                    if (fnRef && ev::isFunction(fnRef->get())) {
                        Value arg = ev::fromUtf8(name);
                        ev::call(fnRef->get(), ev::undefined(), std::span<const Value>(&arg, 1));
                    }
                });
            } else {
                sm->ensurePlayer().setOnFinished(nullptr);
            }
            return ev::undefined();
        });

    b.def("addBlendSpace1D", 2, [](Value self_, std::span<const Value> a) {
        return addBlendSpaceImpl(self_, a, false);
    });

    b.def("addBlendSpace2D", 2, [](Value self_, std::span<const Value> a) {
        return addBlendSpaceImpl(self_, a, true);
    });

    b.def("setBlendPos", 2, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh)
            return ev::throwTypeError("setBlendPos: node is not a skinned mesh");
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (!sm) return ev::throwTypeError("setBlendPos: node is not a skinned mesh");
        if (a.size() < 2 || !ev::isString(a[0]))
            return ev::throwTypeError("setBlendPos(name, x[, y])");
        double x = 0, y = 0;
        if (ev::isObject(a[1])) {
            Value e0 = ev::getElement(a[1], 0);
            Value e1 = ev::getElement(a[1], 1);
            if (ev::isNumber(e0)) x = ev::toDouble(e0);
            if (ev::isNumber(e1)) y = ev::toDouble(e1);
        } else {
            if (ev::isNumber(a[1])) x = ev::toDouble(a[1]);
            if (a.size() > 2 && ev::isNumber(a[2])) y = ev::toDouble(a[2]);
        }
        std::string name = ev::toUtf8(a[0]);
        auto* player = sm->player();
        if (!player || !player->setBlendPos(name, static_cast<float>(x), static_cast<float>(y)))
            return ev::throwTypeError("setBlendPos: unknown blend space '" + name + "'");
        return self_;
    });

    b.def("addStateMachine", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh)
            return ev::throwTypeError("addStateMachine: node is not a skinned mesh");
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (!sm) return ev::throwTypeError("addStateMachine: node is not a skinned mesh");
        if (a.empty() || !ev::isObject(a[0]))
            return ev::throwTypeError("addStateMachine({states, transitions, initial?})");

        scene::AnimationPlayer::StateMachineDef def;
        Value initVal = ev::getProperty(a[0], "initial");
        def.initial = ev::isString(initVal) ? ev::toUtf8(initVal) : "";

        Value statesVal = ev::getProperty(a[0], "states");
        if (ev::isObject(statesVal)) {
            int len = static_cast<int>(ev::toDouble(ev::getProperty(statesVal, "length")));
            for (int i = 0; i < len; ++i) {
                Value e = ev::getElement(statesVal, static_cast<uint32_t>(i));
                scene::AnimationPlayer::StateDef st;
                if (ev::isObject(e)) {
                    Value nm = ev::getProperty(e, "name");
                    Value src = ev::getProperty(e, "source");
                    Value spd = ev::getProperty(e, "speed");
                    Value lp = ev::getProperty(e, "loop");
                    st.name = ev::isString(nm) ? ev::toUtf8(nm) : "";
                    st.source = ev::isString(src) ? ev::toUtf8(src) : "";
                    st.speed = ev::isNumber(spd) ? static_cast<float>(ev::toDouble(spd)) : 1.0f;
                    st.loop = !ev::isUndefined(lp) ? ev::toBool(lp) : true;
                }
                def.states.push_back(std::move(st));
            }
        }

        Value transVal = ev::getProperty(a[0], "transitions");
        if (ev::isObject(transVal)) {
            int len = static_cast<int>(ev::toDouble(ev::getProperty(transVal, "length")));
            for (int i = 0; i < len; ++i) {
                Value e = ev::getElement(transVal, static_cast<uint32_t>(i));
                scene::AnimationPlayer::TransitionDef tr;
                if (ev::isObject(e)) {
                    Value frm = ev::getProperty(e, "from");
                    Value toV = ev::getProperty(e, "to");
                    Value fade = ev::getProperty(e, "fade");
                    Value autoAdv = ev::getProperty(e, "autoAdvance");
                    Value syncPh = ev::getProperty(e, "syncPhase");
                    tr.from = ev::isString(frm) ? ev::toUtf8(frm) : "";
                    tr.to = ev::isString(toV) ? ev::toUtf8(toV) : "";
                    tr.fade = ev::isNumber(fade) ? static_cast<float>(ev::toDouble(fade)) : 0.0f;
                    tr.autoAdvance = !ev::isUndefined(autoAdv) ? ev::toBool(autoAdv) : false;
                    tr.syncPhase = !ev::isUndefined(syncPh) ? ev::toBool(syncPh) : false;
                }
                def.transitions.push_back(std::move(tr));
            }
        }

        std::string err;
        if (!sm->ensurePlayer().setStateMachine(std::move(def), &err))
            return ev::throwTypeError("addStateMachine: " + err);
        return self_;
    });

    b.def("travel", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh)
            return ev::throwTypeError("travel: node is not a skinned mesh");
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (!sm) return ev::throwTypeError("travel: node is not a skinned mesh");
        if (a.empty() || !ev::isString(a[0]))
            return ev::throwTypeError("travel(stateName)");
        std::string name = ev::toUtf8(a[0]);
        auto* player = sm->player();
        if (!player || !player->travel(name))
            return ev::throwTypeError("travel: unknown state '" + name + "' (addStateMachine first)");
        return self_;
    });

    b.accessor("state", [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh) return ev::undefined();
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (sm) {
            auto* player = sm->player();
            if (player && player->hasStateMachine()) {
                const std::string& s = player->currentState();
                return s.empty() ? ev::null() : ev::fromUtf8(s);
            }
            return ev::null();
        }
        return ev::undefined();
    }, nullptr);

    b.accessor("onStateChanged",
        [](Value, std::span<const Value>) { return ev::undefined(); },
        [](Value self_, std::span<const Value> a) {
            auto* n = sceneNodeOf(self_);
            if (!n || n->type() != scene::SceneNode::Type::Mesh) return ev::undefined();
            auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
            if (!sm) return ev::undefined();
            if (!a.empty() && ev::isFunction(a[0])) {
                auto fnRef = std::make_shared<ev::Persistent>(a[0]);
                sm->ensurePlayer().setOnStateChanged([fnRef](const std::string& from, const std::string& to) {
                    if (fnRef && ev::isFunction(fnRef->get())) {
                        Value args[2] = {
                            from.empty() ? ev::null() : ev::fromUtf8(from),
                            ev::fromUtf8(to)
                        };
                        ev::call(fnRef->get(), ev::undefined(), std::span<const Value>(args, 2));
                    }
                });
            } else {
                sm->ensurePlayer().setOnStateChanged(nullptr);
            }
            return ev::undefined();
        });

    b.def("blendState", 0, [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh)
            return ev::throwTypeError("blendState: node is not a skinned mesh");
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (!sm) return ev::throwTypeError("blendState: node is not a skinned mesh");
        auto* player = sm->player();
        ObjectBuilder obj;
        obj.set("state", ev::null());
        Value clips = ev::parseJson("[]").value;
        Value layers = ev::parseJson("[]").value;
        double phase = 0.0;
        if (player) {
            auto s = player->blendState();
            phase = s.phase;
            if (!s.state.empty()) obj.set("state", ev::fromUtf8(s.state));
            for (uint32_t i = 0; i < s.clips.size(); ++i) {
                ObjectBuilder c;
                c.set("name", ev::fromUtf8(s.clips[i].name));
                c.set("weight", ev::fromDouble(s.clips[i].weight));
                ev::setElement(clips, i, c.get());
            }
            if (s.hasPos) {
                Value pos = ev::parseJson("[]").value;
                ev::setElement(pos, 0, ev::fromDouble(s.pos[0]));
                if (s.is2D) ev::setElement(pos, 1, ev::fromDouble(s.pos[1]));
                obj.set("pos", pos);
            }
            for (uint32_t i = 0; i < s.layers.size(); ++i) {
                const auto& L = s.layers[i];
                ObjectBuilder l;
                l.set("slot", ev::fromDouble(L.slot));
                l.set("name", ev::fromUtf8(L.name));
                l.set("weight", ev::fromDouble(L.weight));
                l.set("phase", ev::fromDouble(L.phase));
                ev::setElement(layers, i, l.get());
            }
        }
        obj.set("clips", clips);
        obj.set("phase", ev::fromDouble(phase));
        obj.set("layers", layers);
        return obj.get();
    });

    b.def("playLayer", 2, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh)
            return ev::throwTypeError("playLayer: node is not a skinned mesh");
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (!sm) return ev::throwTypeError("playLayer: node is not a skinned mesh");
        if (a.size() < 2 || !ev::isNumber(a[0]) || !ev::isString(a[1]))
            return ev::throwTypeError("playLayer(slot, clipName[, opts])");
        int slot = static_cast<int>(ev::toDouble(a[0]));
        scene::AnimationPlayer::PlayOptions opts;
        if (a.size() > 2 && ev::isObject(a[2])) readPlayOptions(a[2], opts);
        std::string name = ev::toUtf8(a[1]);
        if (!sm->ensurePlayer().playLayer(slot, name, opts)) {
            return ev::throwTypeError("playLayer: bad slot " + std::to_string(slot) +
                ", unknown clip '" + name + "', or no skeleton");
        }
        return self_;
    });

    b.def("stopLayer", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh)
            return ev::throwTypeError("stopLayer: node is not a skinned mesh");
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (!sm) return ev::throwTypeError("stopLayer: node is not a skinned mesh");
        if (a.empty() || !ev::isNumber(a[0]))
            return ev::throwTypeError("stopLayer(slot[, {fadeTime}])");
        float fade = 0.0f;
        if (a.size() > 1 && ev::isObject(a[1])) {
            Value fVal = ev::getProperty(a[1], "fadeTime");
            if (ev::isNumber(fVal)) fade = static_cast<float>(ev::toDouble(fVal));
        }
        if (auto* player = sm->player())
            player->stopLayer(static_cast<int>(ev::toDouble(a[0])), fade);
        return self_;
    });

    b.def("setLayerWeight", 2, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh)
            return ev::throwTypeError("setLayerWeight: node is not a skinned mesh");
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (!sm) return ev::throwTypeError("setLayerWeight: node is not a skinned mesh");
        if (a.size() < 2 || !ev::isNumber(a[0]) || !ev::isNumber(a[1]))
            return ev::throwTypeError("setLayerWeight(slot, weight)");
        int slot = static_cast<int>(ev::toDouble(a[0]));
        auto* player = sm->player();
        if (!player || !player->setLayerWeight(slot, static_cast<float>(ev::toDouble(a[1]))))
            return ev::throwTypeError("setLayerWeight: no active layer in slot " + std::to_string(slot));
        return self_;
    });

    b.def("setRootMotion", 1, [](Value self_, std::span<const Value> a) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh)
            return ev::throwTypeError("setRootMotion: node is not a skinned mesh");
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (!sm) return ev::throwTypeError("setRootMotion: node is not a skinned mesh");
        if (a.empty() || !ev::isObject(a[0]))
            return ev::throwTypeError("setRootMotion({enabled, bone?, extractY?})");

        scene::AnimationPlayer::RootMotionOptions opts;
        Value enVal = ev::getProperty(a[0], "enabled");
        if (!ev::isUndefined(enVal)) opts.enabled = ev::toBool(enVal);
        Value eyVal = ev::getProperty(a[0], "extractY");
        if (!ev::isUndefined(eyVal)) opts.extractY = ev::toBool(eyVal);
        Value boneVal = ev::getProperty(a[0], "bone");
        if (ev::isString(boneVal)) opts.boneName = ev::toUtf8(boneVal);
        else if (ev::isNumber(boneVal)) opts.bone = static_cast<int>(ev::toDouble(boneVal));

        if (!sm->ensurePlayer().setRootMotion(opts))
            return ev::throwTypeError("setRootMotion: no skeleton (setSkeleton first) or unknown bone");
        return self_;
    });

    b.def("consumeRootMotion", 0, [](Value self_, std::span<const Value>) {
        auto* n = sceneNodeOf(self_);
        if (!n || n->type() != scene::SceneNode::Type::Mesh)
            return ev::throwTypeError("consumeRootMotion: node is not a skinned mesh");
        auto* sm = static_cast<scene::MeshNode*>(n)->asSkinnedMesh();
        if (!sm) return ev::throwTypeError("consumeRootMotion: node is not a skinned mesh");
        scene::AnimationPlayer::RootMotionDelta d;
        if (auto* player = sm->player()) d = player->consumeRootMotion();
        ObjectBuilder obj;
        Value t = ev::parseJson("[]").value;
        for (uint32_t i = 0; i < 3; ++i) {
            ev::setElement(t, i, ev::fromDouble(d.translation[i]));
        }
        obj.set("translation", t);
        obj.set("yaw", ev::fromDouble(d.yaw));
        return obj.get();
    });
}

} // namespace bro::bronze_host

#endif // BRO_WITH_3D
