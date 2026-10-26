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
    over_ = timing.frameMs > ceiling();
    overTarget_ = timing.frameMs > config_.targetMs;
    // (The headroom is the surprise at the target: while behind, frames run
    // over the target by design and say nothing of it; nor does a frame over
    // the ceiling whatever the service did, its own need past it. A second
    // of the planet's march at 3.3 ms from orbit raised the headroom 0.3 ms,
    // and at its fall of 5e-5 a frame held the budget at its floor for the
    // next twenty seconds, land tiles 16 px behind.)
    const float q = config_.overRate;
    if (!behind_ && !(over_ && timing.frameMs - service > ceiling()))
        headroom_ = std::clamp(headroom_ + config_.headroomStep * (over_ ? 1.0f - q : -q), 0.0f, config_.targetMs);
    need_.push_back(std::max(timing.frameMs - service, 0.0));
    const size_t window = static_cast<size_t>(std::max({config_.window, config_.catchUpWindow, 1}));
    if (need_.size() > window) need_.erase(need_.begin(), need_.end() - static_cast<std::ptrdiff_t>(window));
}

void WorldService::beginFrame() {
    offers_.clear();
    for (auto& g : granted_) g.clear();
    stats_ = {};
    for (Kind& k : kinds_) k.queued = 0, k.worstError = 0.0f;
    float budget = config_.floorMs;
    const auto worst = [&](int frames) {
        const size_t n = std::min(need_.size(), static_cast<size_t>(std::max(frames, 1)));
        return *std::max_element(need_.end() - static_cast<std::ptrdiff_t>(n), need_.end());
    };
    // Keeping up: the target, less the need over the short window.
    if (!need_.empty() && !overTarget_)
        budget = std::max(budget, static_cast<float>(config_.targetMs - headroom_ - worst(config_.window)));
    // Over it whatever the world does: still a share of the frame, so a view
    // that costs past the target (the planet's 1-40 km at 4-7 ms) does not
    // starve its world, missing tiles and all, for 0.1 ms a frame more.
    if (!need_.empty() && overTarget_)
        budget = std::max(budget, static_cast<float>(config_.floorShare * worst(config_.window)));
    // Catching up: the ceiling, less the need over the longer catch-up
    // window (the frame's own spikes, a plant page placed or a pipeline
    // compiled every few dozen frames, fall between the short window's
    // frames, and the ceiling stacked on them ran frames to 8 ms). Never
    // less than keeping up would spend: where those spikes reach most of
    // the ceiling (the planet's region view, 3 ms of 3.9), the long window
    // alone held the budget to 0.5 ms where keeping up spent 1.55, and its
    // regions came in slower behind than they had in step.
    if (behind_ && !need_.empty() && !over_)
        budget = std::max(budget, static_cast<float>(ceiling() - headroom_ - worst(config_.catchUpWindow)));
    if (settle_ > 0) {
        budget = std::max(budget, config_.settleMs);
        --settle_;
    } else if (s_settlingAll.load(std::memory_order_relaxed)) {
        budget = std::max(budget, config_.settleMs);
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
    // Each kind's first (after the required) ahead of the rest.
    {
        std::vector<bool> led(kinds_.size(), false);
        std::vector<WorldJob> lead, rest;
        lead.reserve(offers_.size());
        for (const WorldJob& j : offers_) {
            if (j.required) {
                lead.push_back(j);
                continue;
            }
            const size_t k = static_cast<size_t>(j.kind);
            (led[k] ? rest : lead).push_back(j);
            led[k] = true;
        }
        lead.insert(lead.end(), rest.begin(), rest.end());
        offers_.swap(lead);
    }
    // What is drawn at once (after the carry), what is drawn next frame, and
    // what the jobs drawn at once that the carry kept out hold back for it.
    float now = 0.0f, next = 0.0f, held = 0.0f;
    bool behind = false;
    std::vector<bool> offered(kinds_.size(), false);
    for (const WorldJob& j : offers_) {
        Kind& kind = kinds_[static_cast<size_t>(j.kind)];
        const float c = kind.cost * static_cast<float>(std::max(j.units, 0.0));
        const bool fits = kind.desc.nextFrame ? now + held + next + c <= budget_ : carried_ + now + held + c <= budget_;
        const bool starved =
            !offered[static_cast<size_t>(j.kind)] && kind.starved >= config_.starveFrames && c <= 4.0f * budget_;
        offered[static_cast<size_t>(j.kind)] = true;
        if (!j.required && !fits && !starved) {
            if (!kind.desc.nextFrame && now + held + c <= budget_) held += c;
            ++stats_.queued;
            stats_.worstError = std::max(stats_.worstError, j.error);
            ++kind.queued;
            kind.worstError = std::max(kind.worstError, j.error);
            if (kind.desc.catchUp && j.error > config_.catchUpError) behind = true;
            continue;
        }
        (kind.desc.nextFrame ? next : now) += c;
        granted_[static_cast<size_t>(j.kind)].push_back(j);
        ++stats_.granted;
    }
    for (size_t k = 0; k < kinds_.size(); ++k) {
        Kind& kind = kinds_[k];
        kind.starved = !offered[k] || !granted_[k].empty() ? 0 : kind.starved + 1;
        // (A starved kind's price falls until its first job is granted and
        // measured again.)
        if (kind.starved >= config_.starveFrames) kind.cost = std::max(kind.cost * config_.starveDecay, 1e-9f);
    }
    if (settle_ > 0) {
        quiet_ = stats_.worstError <= config_.settleError ? quiet_ + 1 : 0;
        if (quiet_ >= config_.settleQuiet) settle_ = 0;
    }
    behind_ = behind;
    stats_.catchingUp = behind;
    stats_.spentMs = now + next;
    stats_.carriedMs = carried_;
    carried_ = next;
}

std::span<const WorldJob> WorldService::granted(int k) const {
    if (k < 0 || k >= kindCount()) return {};
    return granted_[static_cast<size_t>(k)];
}

}  // namespace bro::scene
