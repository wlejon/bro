#pragma once

// Plant movers: bodies that push through shrubs and low foliage (a walker
// in a boxwood), so the plants part round them and spring back after. The
// GPU half is vegMoverPush in shaders/scene_vegetation.glsl.
//
// The movers are the grass's (GrassNode::setMover: a body moving through
// the grass moves through whatever grows there). Each keeps a TRAIL: its
// position sampled at fixed times on the wind clock, every kStep seconds,
// the last kTrail of them. A plant's point is moved by the push of every
// sample (away from the body's axis, as far as the body is deep in it)
// through a damped spring's impulse response at the sample's age, so the
// plant parts as the body arrives, holds while it stays, and swings back
// past rest once and settles after it leaves. Nothing is stored per plant:
// the response is a sum over the trail, smooth in time because a sample
// enters with the response's zero at age 0 and leaves at kTrail * kStep,
// where it has died away (and a window takes the rest).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace bro::scene {

class PlantMovers {
public:
    static constexpr int kMovers = 4;
    static constexpr int kTrail = 20;
    static constexpr double kStep = 0.1;   // s between trail samples

    struct Body {
        uint32_t id = 0;
        float x = 0.0f, z = 0.0f;   // absolute world, now
        float radius = 0.5f;        // its push's radius (m)
        float height = 1.8f;        // what it reaches: points this far above their plant's root (m)
        float strength = 1.0f;
    };

    /// Advance to wind-clock time `now` with the bodies where they are now.
    /// A jump of the clock (or a step back) starts every trail afresh.
    void update(double now, const std::vector<Body>& bodies) {
        const bool jump = !(now > last_) || now - last_ > 1.0;
        std::vector<Track> next;
        for (const Body& b : bodies) {
            if (static_cast<int>(next.size()) >= kMovers) break;
            Track t;
            auto it = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& q) { return q.body.id == b.id; });
            if (it != tracks_.end() && !jump) {
                t = *it;
                // Samples at the fixed times passed since the last update,
                // where the body was then (between its last spot and this).
                const double t0 = last_;
                while (t.next <= now) {
                    const double u = now > t0 ? std::clamp((t.next - t0) / (now - t0), 0.0, 1.0) : 1.0;
                    Sample s;
                    s.x = static_cast<float>(t.body.x + (b.x - t.body.x) * u);
                    s.z = static_cast<float>(t.body.z + (b.z - t.body.z) * u);
                    s.time = t.next;
                    t.trail.push_back(s);
                    t.next += kStep;
                }
                while (static_cast<int>(t.trail.size()) > kTrail) t.trail.erase(t.trail.begin());
            } else {
                t.next = (std::floor(now / kStep) + 1.0) * kStep;
            }
            t.body = b;
            next.push_back(t);
        }
        tracks_.swap(next);
        last_ = now;
    }

    struct Sample { float x = 0.0f, z = 0.0f; double time = 0.0; };
    struct Track {
        Body body;
        std::vector<Sample> trail;   // oldest first
        double next = 0.0;           // the next sample's time
    };
    const std::vector<Track>& tracks() const { return tracks_; }
    double time() const { return last_; }

private:
    std::vector<Track> tracks_;
    double last_ = -1.0;
};

}  // namespace bro::scene
