#include "scene/tween.h"
#include "scene/scene_graph.h"

#include <bromath/easing.h>
#include <bromath/scalar.h>

#include <algorithm>

namespace bro::scene {

// ---------------------------------------------------------------------------
// Easing
// ---------------------------------------------------------------------------

bool Tween::easeFromString(const std::string& name, Ease& out) {
    struct Entry { const char* name; Ease ease; };
    static const Entry kTable[] = {
        {"linear",       Ease::Linear},
        {"quadIn",       Ease::QuadIn},    {"quadOut",    Ease::QuadOut},    {"quadInOut",    Ease::QuadInOut},
        {"cubicIn",      Ease::CubicIn},   {"cubicOut",   Ease::CubicOut},   {"cubicInOut",   Ease::CubicInOut},
        {"quartIn",      Ease::QuartIn},   {"quartOut",   Ease::QuartOut},   {"quartInOut",   Ease::QuartInOut},
        {"quintIn",      Ease::QuintIn},   {"quintOut",   Ease::QuintOut},   {"quintInOut",   Ease::QuintInOut},
        {"sineIn",       Ease::SineIn},    {"sineOut",    Ease::SineOut},    {"sineInOut",    Ease::SineInOut},
        {"expoIn",       Ease::ExpoIn},    {"expoOut",    Ease::ExpoOut},    {"expoInOut",    Ease::ExpoInOut},
        {"circIn",       Ease::CircIn},    {"circOut",    Ease::CircOut},    {"circInOut",    Ease::CircInOut},
        {"backIn",       Ease::BackIn},    {"backOut",    Ease::BackOut},    {"backInOut",    Ease::BackInOut},
        {"elasticIn",    Ease::ElasticIn}, {"elasticOut", Ease::ElasticOut}, {"elasticInOut", Ease::ElasticInOut},
        {"bounceIn",     Ease::BounceIn},  {"bounceOut",  Ease::BounceOut},  {"bounceInOut",  Ease::BounceInOut},
    };
    for (const auto& e : kTable) {
        if (name == e.name) { out = e.ease; return true; }
    }
    return false;
}

float Tween::applyEase(Ease e, float t) {
    using namespace bromath;
    switch (e) {
        case Ease::Linear:       return easeLinear(t);
        case Ease::QuadIn:       return easeQuadIn(t);
        case Ease::QuadOut:      return easeQuadOut(t);
        case Ease::QuadInOut:    return easeQuadInOut(t);
        case Ease::CubicIn:      return easeCubicIn(t);
        case Ease::CubicOut:     return easeCubicOut(t);
        case Ease::CubicInOut:   return easeCubicInOut(t);
        case Ease::QuartIn:      return easeQuartIn(t);
        case Ease::QuartOut:     return easeQuartOut(t);
        case Ease::QuartInOut:   return easeQuartInOut(t);
        case Ease::QuintIn:      return easeQuintIn(t);
        case Ease::QuintOut:     return easeQuintOut(t);
        case Ease::QuintInOut:   return easeQuintInOut(t);
        case Ease::SineIn:       return easeSineIn(t);
        case Ease::SineOut:      return easeSineOut(t);
        case Ease::SineInOut:    return easeSineInOut(t);
        case Ease::ExpoIn:       return easeExpoIn(t);
        case Ease::ExpoOut:      return easeExpoOut(t);
        case Ease::ExpoInOut:    return easeExpoInOut(t);
        case Ease::CircIn:       return easeCircIn(t);
        case Ease::CircOut:      return easeCircOut(t);
        case Ease::CircInOut:    return easeCircInOut(t);
        case Ease::BackIn:       return easeBackIn(t);
        case Ease::BackOut:      return easeBackOut(t);
        case Ease::BackInOut:    return easeBackInOut(t);
        case Ease::ElasticIn:    return easeElasticIn(t);
        case Ease::ElasticOut:   return easeElasticOut(t);
        case Ease::ElasticInOut: return easeElasticInOut(t);
        case Ease::BounceIn:     return easeBounceIn(t);
        case Ease::BounceOut:    return easeBounceOut(t);
        case Ease::BounceInOut:  return easeBounceInOut(t);
    }
    return t;
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

void Tween::addAnims(std::vector<Anim> anims) {
    if (anims.empty()) return;
    bool merge = parallelNext_ && !steps_.empty() && !steps_.back().call;
    parallelNext_ = false;
    if (merge) {
        auto& dst = steps_.back().anims;
        dst.insert(dst.end(), std::make_move_iterator(anims.begin()),
                   std::make_move_iterator(anims.end()));
    } else {
        steps_.push_back(Step{std::move(anims), nullptr});
    }
}

void Tween::addCall(std::function<void()> fn) {
    parallelNext_ = false;
    steps_.push_back(Step{{}, std::move(fn)});
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

void Tween::start() {
    resetProgress();
    finished_ = false;
    paused_ = false;
    running_ = !destroyed_;
    generation_++;
}

void Tween::stop() {
    running_ = false;
    paused_ = false;
    resetProgress();
}

void Tween::resetProgress() {
    stepIndex_ = 0;
    stepElapsed_ = 0.0f;
    loopsDone_ = 0;
    for (auto& s : steps_)
        for (auto& a : s.anims) a.started = false;
}

void Tween::finish() {
    running_ = false;
    finished_ = true;
    if (onFinished_) {
        auto cb = onFinished_; // callback may restart or destroy the tween
        cb();
    }
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

float Tween::stepLength(const Step& s) {
    float len = 0.0f;
    for (const auto& a : s.anims)
        len = std::max(len, a.delay + a.duration);
    return len;
}

void Tween::tick(float dtSec, SceneGraph& graph) {
    if (!running_ || paused_ || dtSec <= 0.0f) return;

    float remaining = dtSec;
    // Safety cap: a loop of only zero-length steps consumes no time and would
    // spin forever; 1024 step transitions per tick is beyond any sane chain.
    int safety = 1024;
    while (running_ && safety-- > 0) {
        if (stepIndex_ >= steps_.size()) {
            loopsDone_++;
            if (steps_.empty() || (loops_ >= 0 && loopsDone_ >= loops_)) {
                finish();
                return;
            }
            // Next loop iteration: re-capture start values so each pass
            // tweens from wherever the properties currently are.
            for (auto& s : steps_)
                for (auto& a : s.anims) a.started = false;
            stepIndex_ = 0;
            stepElapsed_ = 0.0f;
            continue;
        }

        Step& s = steps_[stepIndex_];
        if (s.call) {
            auto fn = s.call; // callback may stop()/start()/destroy this tween
            uint64_t gen = generation_;
            fn();
            if (!running_ || destroyed_) return;
            if (generation_ != gen) continue; // restarted from the callback
            stepIndex_++;
            continue;
        }

        float len = stepLength(s);
        float newElapsed = stepElapsed_ + remaining;
        bool complete = newElapsed >= len;
        stepElapsed_ = complete ? len : newElapsed;
        applyStep(s, graph);
        if (!complete) return;
        remaining = newElapsed - len;
        stepIndex_++;
        stepElapsed_ = 0.0f;
    }
}

void Tween::applyStep(Step& s, SceneGraph& graph) {
    for (auto& a : s.anims) {
        float local = stepElapsed_ - a.delay;
        if (local < 0.0f) continue;
        float t = a.duration > 0.0f
            ? std::min(local / a.duration, 1.0f) : 1.0f;

        SceneNode* node = a.nodeId ? graph.findById(a.nodeId) : nullptr;

        if (!a.started) {
            a.started = true;
            if (node) {
                switch (a.prop) {
                    case Prop::Position:   a.v3From = node->position(); break;
                    case Prop::Quaternion: a.qFrom = node->rotation();  break;
                    case Prop::Scale:      a.v3From = node->scale();    break;
                    case Prop::Opacity:
                        if (node->type() == SceneNode::Type::Sprite)
                            a.fFrom = static_cast<SpriteNode*>(node)->opacity();
                        else if (node->type() == SceneNode::Type::Mesh)
                            a.fFrom = static_cast<MeshNode*>(node)->color()[3];
                        break;
                    case Prop::Color:
                        if (node->type() == SceneNode::Type::Mesh) {
                            const float* c = static_cast<MeshNode*>(node)->color();
                            a.v3From = {c[0], c[1], c[2]};
                        } else if (node->type() == SceneNode::Type::Light) {
                            a.v3From = static_cast<LightNode*>(node)->color();
                        } else if (node->type() == SceneNode::Type::Shape) {
                            auto c = static_cast<ShapeNode*>(node)->fillColor();
                            a.v3From = {c.r, c.g, c.b};
                        }
                        break;
                    case Prop::Custom: break;
                }
            }
        }

        float e = applyEase(a.ease, t);

        if (node) {
            switch (a.prop) {
                case Prop::Position:
                    node->setPosition(bromath::vlerp(a.v3From, a.v3To, e));
                    break;
                case Prop::Quaternion:
                    node->setRotation(bromath::qslerp(a.qFrom, a.qTo, e));
                    break;
                case Prop::Scale:
                    node->setScale(bromath::vlerp(a.v3From, a.v3To, e));
                    break;
                case Prop::Opacity: {
                    float v = bromath::lerp(a.fFrom, a.fTo, e);
                    if (node->type() == SceneNode::Type::Sprite) {
                        static_cast<SpriteNode*>(node)->setOpacity(v);
                    } else if (node->type() == SceneNode::Type::Mesh) {
                        auto* m = static_cast<MeshNode*>(node);
                        const float* c = m->color();
                        m->setColor(c[0], c[1], c[2], v);
                    }
                    break;
                }
                case Prop::Color: {
                    bromath::Vec3 v = bromath::vlerp(a.v3From, a.v3To, e);
                    if (node->type() == SceneNode::Type::Mesh) {
                        auto* m = static_cast<MeshNode*>(node);
                        m->setColor(v.x, v.y, v.z, m->color()[3]);
                    } else if (node->type() == SceneNode::Type::Light) {
                        static_cast<LightNode*>(node)->setColor(v.x, v.y, v.z);
                    } else if (node->type() == SceneNode::Type::Shape) {
                        auto* sh = static_cast<ShapeNode*>(node);
                        auto c = sh->fillColor();
                        sh->setFillColor({v.x, v.y, v.z, c.a});
                    }
                    break;
                }
                case Prop::Custom: break;
            }
        }

        if (a.onUpdate) a.onUpdate(e);
    }
}

} // namespace bro::scene
