#include "scene/world/world_service.h"

#include <algorithm>
#include <cmath>

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
        // The first measurement replaces the guess; later ones move it
        // toward themselves by a power of their ratio to it, costQuantile
        // of the rate when dearer and the rest when cheaper.
        const float q = config_.costQuantile;
        k.cost = k.measured ? k.cost * std::pow(sample / k.cost, config_.costRate * (sample > k.cost ? q : 1.0f - q))
                            : sample;
        k.cost = std::max(k.cost, 1e-9f);
        k.measured = true;
    }
    if (timing.frameMs < 0.0) return;
    over_ = timing.frameMs > config_.targetMs;
    const float q = config_.overRate;
    headroom_ = std::clamp(headroom_ + config_.headroomStep * (over_ ? 1.0f - q : -q), 0.0f, config_.targetMs);
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
        budget = std::max(budget, static_cast<float>(config_.targetMs - headroom_ - need));
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
    // What is drawn at once (after the carry), what is drawn next frame, and
    // what the jobs drawn at once that the carry kept out hold back for it.
    float now = 0.0f, next = 0.0f, held = 0.0f;
    for (const WorldJob& j : offers_) {
        const Kind& kind = kinds_[static_cast<size_t>(j.kind)];
        const float c = kind.cost * static_cast<float>(std::max(j.units, 0.0));
        const bool fits = kind.desc.nextFrame ? now + held + next + c <= budget_ : carried_ + now + held + c <= budget_;
        if (!j.required && !fits) {
            if (!kind.desc.nextFrame && now + held + c <= budget_) held += c;
            ++stats_.queued;
            stats_.worstError = std::max(stats_.worstError, j.error);
            continue;
        }
        (kind.desc.nextFrame ? next : now) += c;
        granted_[static_cast<size_t>(j.kind)].push_back(j);
        ++stats_.granted;
    }
    stats_.spentMs = now + next;
    stats_.carriedMs = carried_;
    carried_ = next;
}

std::span<const WorldJob> WorldService::granted(int k) const {
    if (k < 0 || k >= kindCount()) return {};
    return granted_[static_cast<size_t>(k)];
}

}  // namespace bro::scene
