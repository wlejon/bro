#include "scene/world/world_service.h"

#include <algorithm>

namespace bro::scene {

int WorldService::addKind(const WorldJobKind& kind) {
    if (static_cast<int>(kinds_.size()) >= kMaxKinds) return -1;
    kinds_.push_back({kind, std::max(kind.costMs, 1e-9f), false});
    granted_.emplace_back();
    return static_cast<int>(kinds_.size()) - 1;
}

void WorldService::observe(const WorldFrameTiming& timing) {
    double service = 0.0;
    for (const WorldFrameTiming::Span& s : timing.spans) {
        if (s.kind < 0 || s.kind >= kindCount()) continue;
        if (!s.apart) service += s.ms;
        if (s.units <= 0.0 || s.ms <= 0.0) continue;
        Kind& k = kinds_[static_cast<size_t>(s.kind)];
        const float sample = static_cast<float>(s.ms / s.units);
        // The first measurement replaces the guess; later ones are followed.
        k.cost = k.measured ? k.cost + config_.costRate * (sample - k.cost) : sample;
        k.cost = std::max(k.cost, 1e-9f);
        k.measured = true;
    }
    if (timing.frameMs < 0.0) return;
    over_ = timing.frameMs > config_.targetMs;
    need_.push_back(std::max(timing.frameMs - service, 0.0));
    const size_t window = static_cast<size_t>(std::max(config_.window, 1));
    if (need_.size() > window) need_.erase(need_.begin(), need_.end() - static_cast<std::ptrdiff_t>(window));
}

void WorldService::beginFrame() {
    offers_.clear();
    for (auto& g : granted_) g.clear();
    stats_ = {};
    float budget = config_.floorMs;
    if (!need_.empty() && !over_) {
        const double need = *std::max_element(need_.begin(), need_.end());
        budget = std::max(budget, static_cast<float>(config_.targetMs - need));
    }
    budget_ = budget;
    stats_.budgetMs = budget;
}

void WorldService::schedule() {
    stats_.offered = static_cast<uint32_t>(offers_.size());
    // (Offers of an unknown kind are dropped.)
    std::erase_if(offers_, [&](const WorldJob& j) { return j.kind < 0 || j.kind >= kindCount(); });
    std::sort(offers_.begin(), offers_.end(), [](const WorldJob& a, const WorldJob& b) {
        if (a.required != b.required) return a.required;
        if (a.error != b.error) return a.error > b.error;
        if (a.size != b.size) return a.size > b.size;
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.producer != b.producer) return a.producer < b.producer;
        return a.key < b.key;
    });
    float spent = 0.0f;
    for (const WorldJob& j : offers_) {
        const float c = kinds_[static_cast<size_t>(j.kind)].cost * static_cast<float>(std::max(j.units, 0.0));
        if (!j.required && spent + c > budget_) {
            ++stats_.queued;
            stats_.worstError = std::max(stats_.worstError, j.error);
            continue;
        }
        spent += c;
        granted_[static_cast<size_t>(j.kind)].push_back(j);
        ++stats_.granted;
    }
    stats_.spentMs = spent;
}

std::span<const WorldJob> WorldService::granted(int k) const {
    if (k < 0 || k >= kindCount()) return {};
    return granted_[static_cast<size_t>(k)];
}

}  // namespace bro::scene
