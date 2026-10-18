// `__bro_native.models` — the C entry points behind bro.models (docs/models-api.js):
// where model weights live. The search list is util/model_dirs.h's: BRO_MODELS,
// then the `models.dirs` setting, then the per-user models directory. fetch and
// available run util/model_fetch.h's jobs, installing into the per-user directory
// from the `models.sources` setting's datasets. The public shape is js/models.js's.
//
// Main realm only: the setting is the engine's settings store, which only the
// engine's thread reads. A Worker is handed the directory the page found.

#include "bronze_host/host_runtime.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"
#include "engine/settings.h"
#include "natives/models/native_models_decl.h"
#include "util/json_out.h"
#include "util/model_dirs.h"
#include "util/model_fetch.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::string settingString(const char* key) {
    if (auto* eng = bro::bronze_host::hostEngine()) {
        if (auto* s = eng->settings()) return s->getString(key);
    }
    return {};
}

std::vector<std::string> currentDirs() {
    return bro::util::modelDirs(settingString("models.dirs"));
}

// ---- fetch jobs ----------------------------------------------------------------
//
// Each fetchStart answers an id; installs of the same name share one job (one
// staging folder), so two fetch() calls for a model do not race each other.
// A job runs until it settles; one every caller cancelled is asked to stop.
// Settled jobs are destroyed (joined, which is then immediate) on the JS
// thread; one still stopping waits in the registry, never joined from a poll.
// The registry is a cold control path: a mutex is fine (CLAUDE.md threading).

using bro::util::ModelFetchJob;

struct SharedJob {
    std::shared_ptr<ModelFetchJob> job;  // shared only with a successor waiting on it
    int live = 0;  // ids that have not cancelled
};

struct Sub {
    std::shared_ptr<SharedJob> shared;
    bool cancelled = false;
};

struct Registry {
    std::mutex mu;
    int nextId = 1;
    std::map<int, Sub> subs;
    std::vector<std::shared_ptr<SharedJob>> stopping;  // cancelled by every id, not yet settled
    bool hookInstalled = false;
};

Registry& registry() {
    static Registry r;
    return r;
}

// Under the lock: destroy stopping jobs that have settled.
void reapLocked(Registry& r) {
    r.stopping.erase(std::remove_if(r.stopping.begin(), r.stopping.end(),
                                    [](const std::shared_ptr<SharedJob>& s) {
                                        return s->job->snapshot().state != ModelFetchJob::State::Running;
                                    }),
                     r.stopping.end());
}

void shutdownFetches() {
    Registry& r = registry();
    std::vector<std::shared_ptr<SharedJob>> all;
    {
        std::lock_guard<std::mutex> lk(r.mu);
        for (auto& [id, sub] : r.subs) all.push_back(sub.shared);
        for (auto& s : r.stopping) all.push_back(s);
        r.subs.clear();
        r.stopping.clear();
    }
    for (auto& s : all) s->job->cancel();
    all.clear();  // the last reference joins each thread
}

const char* stateName(ModelFetchJob::State s) {
    switch (s) {
        case ModelFetchJob::State::Running: return "running";
        case ModelFetchJob::State::Done: return "done";
        case ModelFetchJob::State::Failed: return "failed";
        case ModelFetchJob::State::Cancelled: return "cancelled";
    }
    return "failed";
}

}  // namespace

extern "C" {

const char* bro_models_dirs(void) {
    std::string joined;
    for (const auto& d : currentDirs()) {
        if (!joined.empty()) joined += '\n';
        joined += d;
    }
    return bro::bronze_host::natives::strResult(std::move(joined));
}

const char* bro_models_find(const char* name) {
    return bro::bronze_host::natives::strResult(
        bro::util::findModel(name ? name : "", currentDirs()));
}

const char* bro_models_sources(void) {
    std::string joined;
    for (const auto& s : bro::util::parseModelSources(settingString("models.sources"))) {
        if (!joined.empty()) joined += '\n';
        joined += s.label;
    }
    return bro::bronze_host::natives::strResult(std::move(joined));
}

int32_t bro_models_fetchStart(const char* name, bool listOnly) {
    const std::string n = name ? name : "";
    if (!bro::util::isModelName(n)) return 0;
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.mu);
    reapLocked(r);
    if (!r.hookInstalled) {
        if (auto* eng = bro::bronze_host::hostEngine()) {
            eng->addShutdownHook([] { shutdownFetches(); });
            r.hookInstalled = true;
        }
    }
    std::shared_ptr<SharedJob> shared;
    if (!listOnly) {
        for (auto& [id, sub] : r.subs) {
            const auto& j = sub.shared->job;
            if (!j->listOnly() && j->name() == n && sub.shared->live > 0 &&
                j->snapshot().state == ModelFetchJob::State::Running) {
                shared = sub.shared;
                break;
            }
        }
    }
    if (!shared) {
        // A cancelled install of the same folder may still be stopping: the
        // new one waits for it rather than share its staging files.
        std::shared_ptr<ModelFetchJob> after;
        if (!listOnly) {
            auto stoppingInstall = [&](const std::shared_ptr<SharedJob>& s) {
                return !s->job->listOnly() && s->job->name() == n &&
                       s->job->snapshot().state == ModelFetchJob::State::Running;
            };
            for (auto& s : r.stopping) if (stoppingInstall(s)) after = s->job;
            for (auto& [id, sub] : r.subs) if (sub.shared->live == 0 && stoppingInstall(sub.shared)) after = sub.shared->job;
        }
        shared = std::make_shared<SharedJob>();
        shared->job = std::make_shared<ModelFetchJob>(
            n, bro::util::userModelsDir(),
            bro::util::parseModelSources(settingString("models.sources")), listOnly, std::move(after));
    }
    ++shared->live;
    const int id = r.nextId++;
    r.subs[id] = Sub{shared, false};
    return id;
}

const char* bro_models_fetchPoll(int32_t id) {
    Registry& r = registry();
    bro::util::JsonOut j;
    std::shared_ptr<SharedJob> drop;  // destroyed after the lock is released
    {
        std::lock_guard<std::mutex> lk(r.mu);
        reapLocked(r);
        auto it = r.subs.find(id);
        if (it == r.subs.end()) {
            j.beginObject();
            j.key("state").string("failed");
            j.key("code").string("io");
            j.key("error").string("no such fetch");
            j.endObject();
            return bro::bronze_host::natives::strResult(j.str());
        }
        const ModelFetchJob::Snapshot s = it->second.shared->job->snapshot();
        const bool cancelled = it->second.cancelled;
        const char* state = cancelled ? "cancelled" : stateName(s.state);
        j.beginObject();
        j.key("state").string(state);
        j.key("done").integer(static_cast<int64_t>(s.done));
        j.key("total").integer(static_cast<int64_t>(s.total));
        j.key("files").integer(s.files);
        j.key("file").string(s.file);
        j.key("source").string(s.source);
        j.key("dir").string(s.dir);
        j.key("error").string(s.error);
        j.key("code").string(s.code);
        j.endObject();
        if (cancelled || s.state != ModelFetchJob::State::Running) {
            drop = std::move(it->second.shared);
            r.subs.erase(it);
            // A job other ids still follow, or one still stopping, is kept.
            if (drop.use_count() > 1) drop.reset();
            else if (drop->job->snapshot().state == ModelFetchJob::State::Running) {
                r.stopping.push_back(std::move(drop));
            }
        }
    }
    return bro::bronze_host::natives::strResult(j.str());
}

void bro_models_fetchCancel(int32_t id) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.mu);
    auto it = r.subs.find(id);
    if (it == r.subs.end() || it->second.cancelled) return;
    it->second.cancelled = true;
    if (--it->second.shared->live == 0) it->second.shared->job->cancel();
}

}  // extern "C"

namespace bro::bronze_host {


}  // namespace bro::bronze_host
