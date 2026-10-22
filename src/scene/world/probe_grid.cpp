#include "scene/world/probe_grid.h"

#include <algorithm>
#include <cmath>

namespace bro::scene {

void ProbeGrid::configure(const ProbeGridConfig& config) {
    config_ = config;
    config_.cascades = std::max(config_.cascades, 1);
    config_.columns = std::max(config_.columns, 2);
    config_.layers = std::max(config_.layers, 2);
    config_.sliceRows = std::clamp(config_.sliceRows, 1, config_.columns);
    config_.freshColumns = std::max(config_.freshColumns, 1);
    cascades_.assign(static_cast<size_t>(config_.cascades), Cascade{});
    const int slices = (config_.columns + config_.sliceRows - 1) / config_.sliceRows;
    // The slices start spread over their period, so they come due one by one
    // (and stay so), not a cascade's at once.
    for (size_t i = 0; i < cascades_.size(); ++i) {
        const float period = config_.refreshSeconds * static_cast<float>(1 << i);
        Cascade& c = cascades_[i];
        c.sliceAge.resize(static_cast<size_t>(slices));
        for (int s = 0; s < slices; ++s)
            c.sliceAge[static_cast<size_t>(s)] = -period * static_cast<float>(s) / static_cast<float>(slices);
    }
    jobs_.clear();
}

void ProbeGrid::setTop(float metres) { top_ = std::max(metres, kBottom * 4.0f); }

float ProbeGrid::layerHeight(int k) const {
    const float t = static_cast<float>(std::clamp(k, 0, config_.layers - 1)) / static_cast<float>(config_.layers - 1);
    return kBottom + (top_ - kBottom) * std::pow(t, kLayerPower);
}

float ProbeGrid::layerAt(float height) const {
    const float t = std::clamp((height - kBottom) / (top_ - kBottom), 0.0f, 1.0f);
    return std::pow(t, 1.0f / kLayerPower) * static_cast<float>(config_.layers - 1);
}

double ProbeGrid::spacing(int c) const {
    return static_cast<double>(config_.spacing) * std::pow(static_cast<double>(config_.ratio), c);
}

void ProbeGrid::queueFresh(Cascade& c, const ProbeBox& box) const {
    // Tiles as wide as the box, or the most a box holds, and as many rows
    // as that leaves.
    const int w = std::min(box.x1 - box.x0, config_.freshColumns);
    const int h = std::max(config_.freshColumns / std::max(w, 1), 1);
    for (int z0 = box.z0; z0 < box.z1; z0 += h)
        for (int x0 = box.x0; x0 < box.x1; x0 += w) {
            ProbeBox b = box;
            b.x0 = x0;
            b.x1 = std::min(x0 + w, box.x1);
            b.z0 = z0;
            b.z1 = std::min(z0 + h, box.z1);
            c.fresh.push_back(b);
        }
}

bool ProbeGrid::seen(const ProbeBox& b) const {
    if (b.cascade + 1 >= config_.cascades) return true;
    const int n = config_.columns;
    const bool edgeX = b.x1 - b.x0 == 1 && (b.x0 == 0 || b.x0 == n - 1);
    const bool edgeZ = b.z1 - b.z0 == 1 && (b.z0 == 0 || b.z0 == n - 1);
    return !edgeX && !edgeZ;
}

bool ProbeGrid::madeOver(const Cascade& c, const ProbeBox& box) const {
    if (!c.made || c.madeSun != sun_ || c.madeTop != top_) return false;
    return std::none_of(c.stale.begin(), c.stale.end(), [&](const ProbeBox& s) {
        return s.x0 < box.x1 && box.x0 < s.x1 && s.z0 < box.z1 && box.z0 < s.z1;
    });
}

void ProbeGrid::addStale(Cascade& c, ProbeBox box) {
    // A cascade's stale boxes never overlap (a voxel is splatted once): one
    // that would takes in those it overlaps, until none is left.
    const auto overlaps = [](const ProbeBox& a, const ProbeBox& b) {
        return a.x0 < b.x1 && b.x0 < a.x1 && a.z0 < b.z1 && b.z0 < a.z1;
    };
    for (bool merged = true; merged;) {
        merged = false;
        for (auto it = c.stale.begin(); it != c.stale.end(); ++it) {
            if (!overlaps(*it, box)) continue;
            box.x0 = std::min(box.x0, it->x0);
            box.z0 = std::min(box.z0, it->z0);
            box.x1 = std::max(box.x1, it->x1);
            box.z1 = std::max(box.z1, it->z1);
            c.stale.erase(it);
            merged = true;
            break;
        }
    }
    c.stale.push_back(box);
    if (c.stale.size() <= static_cast<size_t>(kMaxStaleBoxes)) return;
    ProbeBox all = c.stale.front();
    for (const ProbeBox& b : c.stale) {
        all.x0 = std::min(all.x0, b.x0);
        all.z0 = std::min(all.z0, b.z0);
        all.x1 = std::max(all.x1, b.x1);
        all.z1 = std::max(all.z1, b.z1);
    }
    c.stale.assign(1, all);
}

void ProbeGrid::queueMove(Cascade& c, int index, int64_t dx, int64_t dz) {
    const int n = config_.columns;
    // The boxes still queued move with the square; what leaves it goes.
    const auto shift = [&](std::vector<ProbeBox>& boxes) {
        std::vector<ProbeBox> kept;
        for (ProbeBox b : boxes) {
            const int64_t x0 = std::max<int64_t>(b.x0 - dx, 0), x1 = std::min<int64_t>(b.x1 - dx, n);
            const int64_t z0 = std::max<int64_t>(b.z0 - dz, 0), z1 = std::min<int64_t>(b.z1 - dz, n);
            if (x0 >= x1 || z0 >= z1) continue;
            b.x0 = static_cast<int>(x0);
            b.x1 = static_cast<int>(x1);
            b.z0 = static_cast<int>(z0);
            b.z1 = static_cast<int>(z1);
            kept.push_back(b);
        }
        boxes = std::move(kept);
    };
    shift(c.fresh);
    shift(c.stale);
    if (std::abs(dx) >= n || std::abs(dz) >= n) {
        c.fresh.clear();
        queueFresh(c, ProbeBox{index, 0, 0, n, n, true});
        c.made = false;
        c.stale.clear();
        return;
    }
    // The strips the move brought in: the columns along x, then the rows
    // along z (less what the columns took); their world and their probes.
    const int ax = static_cast<int>(std::abs(dx)), az = static_cast<int>(std::abs(dz));
    if (ax > 0) {
        const int x0 = dx > 0 ? n - ax : 0;
        const ProbeBox strip{index, x0, 0, x0 + ax, n, true};
        queueFresh(c, strip);
        addStale(c, strip);
    }
    if (az > 0) {
        const int z0 = dz > 0 ? n - az : 0;
        const int x0 = dx > 0 ? 0 : ax, x1 = dx > 0 ? n - ax : n;
        if (x0 < x1) {
            const ProbeBox strip{index, x0, z0, x1, z0 + az, true};
            queueFresh(c, strip);
            addStale(c, strip);
        }
    }
}

void ProbeGrid::place(double eyeX, double eyeZ) {
    const int n = config_.columns;
    for (int i = 0; i < config_.cascades; ++i) {
        Cascade& c = cascades_[static_cast<size_t>(i)];
        const double s = spacing(i);
        // The eye in the middle column: the corner half the square back.
        const int64_t x = static_cast<int64_t>(std::floor(eyeX / s + 0.5)) - n / 2;
        const int64_t z = static_cast<int64_t>(std::floor(eyeZ / s + 0.5)) - n / 2;
        if (!c.placed) {
            c.placed = true;
            c.cornerX = x;
            c.cornerZ = z;
            c.fresh.clear();
            queueFresh(c, ProbeBox{i, 0, 0, n, n, true});
            continue;
        }
        if (x == c.cornerX && z == c.cornerZ) continue;
        queueMove(c, i, x - c.cornerX, z - c.cornerZ);
        c.cornerX = x;
        c.cornerZ = z;
    }
}

void ProbeGrid::canopyChanged(double x0, double z0, double x1, double z1) {
    // The crowns' shade falls away from the sun, as far as the sun's ray
    // from the ground crosses under the layers' top (a low sun's no
    // farther than twenty tops).
    const float reach = top_ / std::max(sunDir_[1], 0.05f);
    const double sx = -sunDir_[0] * reach, sz = -sunDir_[2] * reach;
    x0 += std::min(sx, 0.0);
    x1 += std::max(sx, 0.0);
    z0 += std::min(sz, 0.0);
    z1 += std::max(sz, 0.0);
    const int n = config_.columns;
    for (size_t i = 0; i < cascades_.size(); ++i) {
        Cascade& c = cascades_[i];
        if (!c.made) continue;
        // Column k's world is [k - 0.5, k + 0.5) spacings from the corner's.
        const double s = spacing(static_cast<int>(i));
        const auto first = [&](double v, int64_t corner) {
            return std::clamp<int64_t>(static_cast<int64_t>(std::floor(v / s + 0.5)) - corner, 0, n);
        };
        const ProbeBox box{static_cast<int>(i), static_cast<int>(first(x0, c.cornerX)),
                           static_cast<int>(first(z0, c.cornerZ)), static_cast<int>(first(x1, c.cornerX - 1)),
                           static_cast<int>(first(z1, c.cornerZ - 1)), false};
        if (box.x0 < box.x1 && box.z0 < box.z1) addStale(c, box);
    }
}

void ProbeGrid::canopyChanged() {
    for (Cascade& c : cascades_) {
        c.made = false;
        c.stale.clear();
    }
}

void ProbeGrid::setSun(float x, float y, float z, uint64_t generation) {
    sunDir_[0] = x;
    sunDir_[1] = y;
    sunDir_[2] = z;
    sun_ = generation;
}

bool ProbeGrid::volumesCurrent(int c) const {
    const Cascade& k = cascades_[static_cast<size_t>(c)];
    return k.made && k.stale.empty() && k.madeSun == sun_ && k.madeTop == top_;
}

bool ProbeGrid::staleSeen(int c) const {
    const Cascade& k = cascades_[static_cast<size_t>(c)];
    if (!k.made || k.madeSun != sun_ || k.madeTop != top_) return true;
    return std::any_of(k.stale.begin(), k.stale.end(), [&](const ProbeBox& b) { return seen(b); });
}

void ProbeGrid::staleBoxes(int c, std::vector<ProbeBox>& out) const {
    out.clear();
    const Cascade& k = cascades_[static_cast<size_t>(c)];
    if (!k.made || k.madeSun != sun_ || k.madeTop != top_) {
        out.push_back(ProbeBox{c, 0, 0, config_.columns, config_.columns, false});
        return;
    }
    out = k.stale;
}

void ProbeGrid::volumesMade(int c) {
    Cascade& k = cascades_[static_cast<size_t>(c)];
    k.made = true;
    k.stale.clear();
    k.madeSun = sun_;
    k.madeTop = top_;
}

void ProbeGrid::advance(float seconds) {
    for (Cascade& c : cascades_)
        for (float& a : c.sliceAge) a += seconds;
}

const std::vector<ProbeGrid::TraceJob>& ProbeGrid::traceJobs() {
    jobs_.clear();
    const int n = config_.columns;
    for (int i = 0; i < config_.cascades; ++i) {
        const Cascade& c = cascades_[static_cast<size_t>(i)];
        for (const ProbeBox& b : c.fresh) {
            if (seen(b)) jobs_.push_back(TraceJob{b, 0.0f, true});
            else if (madeOver(c, b)) jobs_.push_back(TraceJob{b, config_.unseenPixels, false});
        }
        const float weight = config_.refreshPixelsPerSecond / static_cast<float>(1 << i);
        const float period = config_.refreshSeconds * static_cast<float>(1 << i);
        for (size_t s = 0; s < c.sliceAge.size(); ++s) {
            if (c.sliceAge[s] < period) continue;
            const int z0 = static_cast<int>(s) * config_.sliceRows;
            ProbeBox box{i, 0, z0, n, std::min(z0 + config_.sliceRows, n), false};
            jobs_.push_back(TraceJob{box, c.sliceAge[s] * weight, false});
        }
    }
    return jobs_;
}

void ProbeGrid::traced(int index) {
    if (index < 0 || index >= static_cast<int>(jobs_.size())) return;
    const TraceJob& job = jobs_[static_cast<size_t>(index)];
    Cascade& c = cascades_[static_cast<size_t>(job.box.cascade)];
    if (job.box.fresh) {
        auto it = std::find_if(c.fresh.begin(), c.fresh.end(), [&](const ProbeBox& b) {
            return b.x0 == job.box.x0 && b.x1 == job.box.x1 && b.z0 == job.box.z0 && b.z1 == job.box.z1;
        });
        if (it != c.fresh.end()) c.fresh.erase(it);
        return;
    }
    c.sliceAge[static_cast<size_t>(job.box.z0 / config_.sliceRows)] = 0.0f;
}

}  // namespace bro::scene
