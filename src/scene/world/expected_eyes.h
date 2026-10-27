#pragma once
// Where the eye is EXPECTED (scene.expect, docs/scene-api.js): points on the
// body a script says the camera is going to (an autopilot's landing, a
// cutscene's next shot, a teleport), so what the world makes about an eye
// is made there ahead of it, from what the budget leaves once the eye's own
// is done and in room the eye does not need (the eye takes it back). So far
// the land's regions (vulkan/ground/scene_land_regions.h). At most kMax at
// once, the sooner first; each says how much is still to be made there.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace bro::scene {

struct ExpectedEye {
    uint32_t id = 0;
    double body[3] = {0.0, 0.0, 0.0};   // a body point (place.h's toBody)
    double within = std::numeric_limits<double>::infinity();   // seconds until the eye is there
    int pending = -1;                   // what is still to be made there (-1: not looked at yet)
};

class ExpectedEyes {
public:
    static constexpr int kMax = 2;

    /// A new one; 0 when kMax are held.
    uint32_t add(const double body[3], double within) {
        if (static_cast<int>(eyes_.size()) >= kMax) return 0;
        ExpectedEye e;
        e.id = ++next_;
        eyes_.push_back(e);
        set(eyes_.back(), body, within);
        return e.id;
    }
    bool move(uint32_t id, const double body[3], double within) {
        for (ExpectedEye& e : eyes_)
            if (e.id == id) {
                set(e, body, within);
                return true;
            }
        return false;
    }
    void cancel(uint32_t id) {
        eyes_.erase(std::remove_if(eyes_.begin(), eyes_.end(), [&](const ExpectedEye& e) { return e.id == id; }),
                    eyes_.end());
    }
    /// What is still to be made there (-1: not looked at yet, or no such one).
    int pending(uint32_t id) const {
        for (const ExpectedEye& e : eyes_)
            if (e.id == id) return e.pending;
        return -1;
    }
    void setPending(uint32_t id, int pending) {
        for (ExpectedEye& e : eyes_)
            if (e.id == id) e.pending = pending;
    }
    /// The sooner first.
    const std::vector<ExpectedEye>& all() const { return eyes_; }

private:
    void set(ExpectedEye& e, const double body[3], double within) {
        std::copy(body, body + 3, e.body);
        e.within = within;
        e.pending = -1;
        std::stable_sort(eyes_.begin(), eyes_.end(),
                         [](const ExpectedEye& a, const ExpectedEye& b) { return a.within < b.within; });
    }

    std::vector<ExpectedEye> eyes_;
    uint32_t next_ = 0;
};

}  // namespace bro::scene
