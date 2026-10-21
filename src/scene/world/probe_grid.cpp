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
    for (int z0 = box.z0; z0 < box.z1; z0 += config_.sliceRows) {
        ProbeBox b = box;
        b.z0 = z0;
        b.z1 = std::min(z0 + config_.sliceRows, box.z1);
        c.fresh.push_back(b);
    }
}

void ProbeGrid::queueMove(Cascade& c, int index, int64_t dx, int64_t dz) {
    const int n = config_.columns;
    // The boxes still queued move with the square; what leaves it goes.
    std::vector<ProbeBox> kept;
    for (ProbeBox b : c.fresh) {
        const int64_t x0 = std::max<int64_t>(b.x0 - dx, 0), x1 = std::min<int64_t>(b.x1 - dx, n);
        const int64_t z0 = std::max<int64_t>(b.z0 - dz, 0), z1 = std::min<int64_t>(b.z1 - dz, n);
        if (x0 >= x1 || z0 >= z1) continue;
        b.x0 = static_cast<int>(x0);
        b.x1 = static_cast<int>(x1);
        b.z0 = static_cast<int>(z0);
        b.z1 = static_cast<int>(z1);
        kept.push_back(b);
    }
    c.fresh = std::move(kept);
    if (std::abs(dx) >= n || std::abs(dz) >= n) {
        c.fresh.clear();
        queueFresh(c, ProbeBox{index, 0, 0, n, n, true});
        return;
    }
    // The strips the move brought in: the columns along x, then the rows
    // along z (less what the columns took).
    const int ax = static_cast<int>(std::abs(dx)), az = static_cast<int>(std::abs(dz));
    if (ax > 0) {
        const int x0 = dx > 0 ? n - ax : 0;
        queueFresh(c, ProbeBox{index, x0, 0, x0 + ax, n, true});
    }
    if (az > 0) {
        const int z0 = dz > 0 ? n - az : 0;
        const int x0 = dx > 0 ? 0 : ax, x1 = dx > 0 ? n - ax : n;
        if (x0 < x1) queueFresh(c, ProbeBox{index, x0, z0, x1, z0 + az, true});
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
    for (size_t i = 0; i < cascades_.size(); ++i) {
        Cascade& c = cascades_[i];
        const double s = spacing(static_cast<int>(i));
        const double lo[2] = {static_cast<double>(c.cornerX) * s, static_cast<double>(c.cornerZ) * s};
        const double side = s * config_.columns;
        if (x1 < lo[0] || x0 > lo[0] + side || z1 < lo[1] || z0 > lo[1] + side) continue;
        ++c.canopy;
    }
}

void ProbeGrid::canopyChanged() {
    for (Cascade& c : cascades_) ++c.canopy;
}

void ProbeGrid::setSunGeneration(uint64_t generation) { sun_ = generation; }

bool ProbeGrid::volumesCurrent(int c) const {
    const Cascade& k = cascades_[static_cast<size_t>(c)];
    return k.made && k.madeX == k.cornerX && k.madeZ == k.cornerZ && k.madeCanopy == k.canopy && k.madeSun == sun_ &&
           k.madeTop == top_;
}

void ProbeGrid::volumesMade(int c) {
    Cascade& k = cascades_[static_cast<size_t>(c)];
    k.made = true;
    k.madeX = k.cornerX;
    k.madeZ = k.cornerZ;
    k.madeCanopy = k.canopy;
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
        if (!traceable(i)) continue;
        for (const ProbeBox& b : c.fresh) jobs_.push_back(TraceJob{b, 0.0f, true});
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
