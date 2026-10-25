#include "scene/vulkan/frame/scene_shader_warmup.h"

#include "scene/vulkan/frame/scene_cpu_stalls.h"
#include "util/log.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace bro::scene::vk::shader_warmup {

namespace {

struct SetState {
    std::vector<BuiltinSceneShader> stages;
    Overrides overrides;
    std::shared_ptr<SetResult> result = std::make_shared<SetResult>();
    std::promise<std::shared_ptr<const SetResult>> promise;
    SetFuture future;
    std::atomic<int> remaining{0};
};

struct Job {
    std::shared_ptr<SetState> set;
    size_t stage = 0;
};

struct Recorded {
    std::vector<BuiltinSceneShader> stages;
    Overrides overrides;
};

struct Registry {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    int workers = 0;
    std::unordered_map<uint64_t, std::shared_ptr<SetState>> sets;
    // What this run asked for, in order, for the next run's warm-up.
    std::vector<uint64_t> recordedKeys;
    std::vector<Recorded> recorded;
    std::string recordPath;
    bool begun = false;
    Progress progress;
};

Registry& registry() {
    static Registry* r = new Registry();   // (never destroyed: detached workers wait on it)
    return *r;
}

uint64_t setKey(std::span<const BuiltinSceneShader> stages, const Overrides& overrides) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    for (BuiltinSceneShader s : stages) {
        const int v = static_cast<int>(s);
        mix(&v, sizeof v);
    }
    for (const auto& [name, source] : overrides) {
        mix(name.data(), name.size());
        mix("\0", 1);
        mix(source.data(), source.size());
        mix("\1", 1);
    }
    return h;
}

void workerLoop() {
    Registry& r = registry();
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(r.mutex);
            r.wake.wait(lock, [&] { return !r.jobs.empty(); });
            job = std::move(r.jobs.front());
            r.jobs.pop_front();
        }
        SetState& s = *job.set;
        std::string err;
        std::vector<uint32_t> spirv = SceneVkShaderCompiler::compileBuiltinWith(s.stages[job.stage], s.overrides, &err);
        s.result->spirv[job.stage] = std::move(spirv);
        s.result->errs[job.stage] = std::move(err);
        bool last = false;
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            ++r.progress.shadersDone;
            last = --s.remaining == 0;
            if (last) ++r.progress.setsDone;
        }
        if (last) s.promise.set_value(s.result);
    }
}

// Under the lock.
SetFuture queueLocked(Registry& r, uint64_t key, std::span<const BuiltinSceneShader> stages, const Overrides& overrides,
                      bool warmup) {
    if (auto it = r.sets.find(key); it != r.sets.end()) return it->second->future;
    auto s = std::make_shared<SetState>();
    s->stages.assign(stages.begin(), stages.end());
    s->overrides = overrides;
    s->result->spirv.resize(stages.size());
    s->result->errs.resize(stages.size());
    s->future = s->promise.get_future().share();
    s->remaining = static_cast<int>(stages.size());
    r.sets.emplace(key, s);
    ++r.progress.sets;
    if (warmup) ++r.progress.warmupSets;
    r.progress.shaders += static_cast<int>(stages.size());
    if (stages.empty()) {
        ++r.progress.setsDone;
        s->promise.set_value(s->result);
        return s->future;
    }
    for (size_t i = 0; i < stages.size(); ++i) r.jobs.push_back({s, i});
    // About half the cores: the page's own compile and the frame run beside them.
    const unsigned hc = std::max(2u, std::thread::hardware_concurrency());
    const int want = static_cast<int>(std::clamp(hc / 2, 2u, 12u));
    while (r.workers < want) {
        std::thread(workerLoop).detach();
        ++r.workers;
    }
    r.wake.notify_all();
    return s->future;
}

bool warmupEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("BRO_SHADER_WARMUP");
        return !(v && std::strcmp(v, "0") == 0);
    }();
    return on;
}

// The record: "BROSW1\n", then a set at a time: its stages by name, its
// overrides (name, source), each string a u32 length and its bytes.
void putU32(std::string& out, uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); }
void putStr(std::string& out, const std::string& s) {
    putU32(out, static_cast<uint32_t>(s.size()));
    out += s;
}

void writeRecordLocked(const Registry& r) {
    if (r.recordPath.empty()) return;
    std::string out = "BROSW1\n";
    for (const Recorded& rec : r.recorded) {
        putU32(out, static_cast<uint32_t>(rec.stages.size()));
        for (BuiltinSceneShader s : rec.stages) putStr(out, SceneVkShaderCompiler::builtinName(s));
        putU32(out, static_cast<uint32_t>(rec.overrides.size()));
        for (const auto& [name, source] : rec.overrides) {
            putStr(out, name);
            putStr(out, source);
        }
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::u8path(r.recordPath).parent_path(), ec);
    const std::string tmp = r.recordPath + ".tmp";
    {
        std::ofstream f(fs::u8path(tmp), std::ios::binary | std::ios::trunc);
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!f) return;
    }
    fs::rename(fs::u8path(tmp), fs::u8path(r.recordPath), ec);
    if (ec) fs::remove(fs::u8path(tmp), ec);
}

struct Reader {
    const std::string& data;
    size_t at = 0;
    bool ok = true;
    uint32_t u32() {
        if (at + 4 > data.size()) { ok = false; return 0; }
        uint32_t v;
        std::memcpy(&v, data.data() + at, 4);
        at += 4;
        return v;
    }
    std::string str() {
        const uint32_t n = u32();
        if (!ok || at + n > data.size()) { ok = false; return {}; }
        std::string s = data.substr(at, n);
        at += n;
        return s;
    }
};

std::vector<Recorded> readRecord(const std::string& path) {
    std::vector<Recorded> sets;
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return sets;
    const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.compare(0, 7, "BROSW1\n") != 0) return sets;
    std::unordered_map<std::string, BuiltinSceneShader> byName;
    for (int i = 0; i < static_cast<int>(BuiltinSceneShader::Count); ++i) {
        const auto s = static_cast<BuiltinSceneShader>(i);
        byName.emplace(SceneVkShaderCompiler::builtinName(s), s);
    }
    Reader in{data, 7};
    while (in.ok && in.at < data.size()) {
        Recorded rec;
        bool known = true;
        const uint32_t n = in.u32();
        for (uint32_t i = 0; in.ok && i < n; ++i) {
            auto it = byName.find(in.str());
            if (it == byName.end()) known = false;
            else rec.stages.push_back(it->second);
        }
        const uint32_t m = in.u32();
        for (uint32_t i = 0; in.ok && i < m; ++i) {
            std::string name = in.str();
            std::string source = in.str();
            rec.overrides.emplace_back(std::move(name), std::move(source));
        }
        // (A shader this build no longer has: the set is not the one asked for.)
        if (in.ok && known) sets.push_back(std::move(rec));
    }
    return sets;
}

}  // namespace

SetFuture request(std::span<const BuiltinSceneShader> stages, const Overrides& overrides) {
    Registry& r = registry();
    const uint64_t key = setKey(stages, overrides);
    std::lock_guard<std::mutex> lock(r.mutex);
    SetFuture f = queueLocked(r, key, stages, overrides, false);
    if (warmupEnabled() && std::find(r.recordedKeys.begin(), r.recordedKeys.end(), key) == r.recordedKeys.end()) {
        r.recordedKeys.push_back(key);
        r.recorded.push_back({std::vector<BuiltinSceneShader>(stages.begin(), stages.end()), overrides});
        writeRecordLocked(r);
    }
    return f;
}

void begin(const std::string& recordPath) {
    if (!warmupEnabled() || recordPath.empty()) return;
    Registry& r = registry();
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        if (r.begun) return;
        r.begun = true;
    }
    const double t0 = stalls::nowMs();
    std::vector<Recorded> sets = readRecord(recordPath);
    std::lock_guard<std::mutex> lock(r.mutex);
    r.recordPath = recordPath;
    int shaders = 0;
    for (const Recorded& rec : sets) {
        queueLocked(r, setKey(rec.stages, rec.overrides), rec.stages, rec.overrides, true);
        shaders += static_cast<int>(rec.stages.size());
    }
    if (!sets.empty())
        LOG_INFO("Shader warm-up: %zu sets (%d shaders) from the last run queued (%.1f ms)", sets.size(), shaders,
                 stalls::nowMs() - t0);
    // Anything asked for before begin is recorded now.
    if (!r.recorded.empty()) writeRecordLocked(r);
}

namespace {
std::atomic<bool> s_async{false};
}

void setAsync(bool on) {
    const char* v = std::getenv("BRO_ASYNC_SHADERS");
    if (v && *v) on = std::strcmp(v, "0") != 0;
    s_async.store(on, std::memory_order_relaxed);
}

bool async() { return s_async.load(std::memory_order_relaxed); }

Progress progress() {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    return r.progress;
}

}  // namespace bro::scene::vk::shader_warmup
