// bro.app.schedule's records over platform/app_schedule.h (app_schedule.h).
#include "engine/app_schedule.h"

#include "engine/app_runtime.h"
#include "platform/app_schedule.h"
#include "util/json_in.h"
#include "util/json_out.h"
#include "util/log.h"
#include "util/sha.h"
#include "util/user_dirs.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace bro::engine {

namespace {

namespace fs = std::filesystem;

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string u8str(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path recordDir() {
    if (const char* home = std::getenv("BRO_APP_HOME"); home && *home) return u8path(home) / "schedule";
    return u8path(util::userDataDir()) / "schedule";
}

bool g_recordOnly = false;
std::mutex g_mu;
std::vector<ScheduledLaunch> g_recorded;  // record-only runs
std::optional<ScheduledLaunch> g_launchedBy;

std::string sanitize(const std::string& s, size_t max) {
    std::string out;
    for (char c : s) {
        if (out.size() >= max) break;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '-' || c == '_';
        out += ok ? c : '_';
    }
    return out;
}

// One name per (app folder, id): readable, and distinct for two folders
// that declare the same app id.
std::string launchName(const AppRuntimeInfo& app, const std::string& id) {
    util::Sha256 h;
    h.update(app.dir.data(), app.dir.size());
    h.update("\n", 1);
    h.update(id.data(), id.size());
    return "bro-" + sanitize(app.id.empty() ? "app" : app.id, 48) + "-" + sanitize(id, 48) + "-" + h.hex().substr(0, 8);
}

std::string toJson(const ScheduledLaunch& s) {
    util::JsonOut j;
    j.beginObject();
    j.key("name").string(s.name);
    j.key("id").string(s.id);
    j.key("when").integer(s.whenMs);
    j.key("appDir").string(s.appDir);
    j.key("wake").boolean(s.wake);
    j.key("args").beginArray();
    for (const auto& a : s.args) j.string(a);
    j.endArray();
    j.key("command").beginArray();
    for (const auto& a : s.command) j.string(a);
    j.endArray();
    j.endObject();
    return j.str();
}

std::vector<std::string> strings(const util::JsonValue& v) {
    std::vector<std::string> out;
    if (v.isArray())
        for (const auto& e : v.array)
            if (e.isString()) out.push_back(e.string);
    return out;
}

std::optional<ScheduledLaunch> readRecord(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    util::JsonValue v;
    if (!util::parseJson(ss.str(), v) || !v.isObject()) return std::nullopt;
    ScheduledLaunch s;
    s.name = v.get("name").string;
    s.id = v.get("id").string;
    s.whenMs = static_cast<int64_t>(v.get("when").number);
    s.appDir = v.get("appDir").string;
    s.wake = v.get("wake").boolean;
    s.args = strings(v.get("args"));
    s.command = strings(v.get("command"));
    if (s.name.empty()) return std::nullopt;
    return s;
}

bool writeRecord(const ScheduledLaunch& s, std::string& why) {
    std::error_code ec;
    fs::create_directories(recordDir(), ec);
    const fs::path file = recordDir() / (s.name + ".json");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << toJson(s);
    if (!out) {
        why = "writing " + u8str(file) + " failed";
        return false;
    }
    return true;
}

void removeRecord(const std::string& name) {
    std::error_code ec;
    fs::remove(recordDir() / (name + ".json"), ec);
}

}  // namespace

void setScheduleRecordOnly(bool recordOnly) { g_recordOnly = recordOnly; }
bool scheduleRecordOnly() { return g_recordOnly; }

std::string scheduleBackend(std::string* why) {
    if (g_recordOnly) return "recorded";
    return platform::schedule::backend(why);
}

bool isValidScheduleId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    return sanitize(id, 64) == id;
}

bool scheduleAppLaunch(const AppRuntimeInfo& app, const std::string& id, int64_t whenMs,
                       const std::vector<std::string>& args, bool wake, const std::vector<std::string>& launcher,
                       ScheduledLaunch& out, std::string& why) {
    if (!isValidScheduleId(id)) {
        why = "the id must be 1-64 letters, digits, '.', '-' or '_', got '" + id + "'";
        return false;
    }
    if (launcher.empty()) {
        why = "no way to start this app";
        return false;
    }
    ScheduledLaunch s;
    s.name = launchName(app, id);
    s.id = id;
    s.whenMs = whenMs;
    s.args = args;
    s.wake = wake;
    s.appDir = app.dir;
    // The flag right after the executable: bro's and bro-headless's own
    // flags go anywhere, and the launcher may end with the app's folder or a `--`.
    s.command = {launcher[0], "--scheduled", s.name};
    s.command.insert(s.command.end(), launcher.begin() + 1, launcher.end());
    s.command.insert(s.command.end(), args.begin(), args.end());
    if (g_recordOnly) {
        std::lock_guard lock(g_mu);
        g_recorded.erase(std::remove_if(g_recorded.begin(), g_recorded.end(),
                                        [&](const ScheduledLaunch& r) { return r.name == s.name; }),
                         g_recorded.end());
        g_recorded.push_back(s);
        out = s;
        return true;
    }
    platform::schedule::Launch l;
    l.name = s.name;
    l.whenMs = whenMs;
    l.command = s.command;
    l.cwd = app.dir;
    l.wake = wake;
    l.description = (app.manifest.name.empty() ? app.id : app.manifest.name) + ": scheduled launch '" + id +
                    "' (bro.app.schedule)";
    if (!writeRecord(s, why)) return false;
    if (!platform::schedule::add(l, why)) {
        removeRecord(s.name);
        return false;
    }
    out = s;
    return true;
}

bool unscheduleAppLaunch(const AppRuntimeInfo& app, const std::string& id, std::string* why) {
    if (!isValidScheduleId(id)) return false;
    const std::string name = launchName(app, id);
    if (g_recordOnly) {
        std::lock_guard lock(g_mu);
        const size_t before = g_recorded.size();
        g_recorded.erase(std::remove_if(g_recorded.begin(), g_recorded.end(),
                                        [&](const ScheduledLaunch& r) { return r.name == name; }),
                         g_recorded.end());
        return g_recorded.size() != before;
    }
    std::error_code ec;
    const bool had = fs::exists(recordDir() / (name + ".json"), ec) || platform::schedule::exists(name);
    const bool gone = platform::schedule::remove(name, why);
    removeRecord(name);
    return had && gone;
}

std::vector<ScheduledLaunch> scheduledAppLaunches(const AppRuntimeInfo& app) {
    std::vector<ScheduledLaunch> out;
    if (g_recordOnly) {
        std::lock_guard lock(g_mu);
        for (const auto& r : g_recorded)
            if (r.appDir == app.dir) out.push_back(r);
    } else {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(recordDir(), ec)) {
            if (e.path().extension() != ".json") continue;
            std::optional<ScheduledLaunch> r = readRecord(e.path());
            if (!r || r->appDir != app.dir) continue;
            if (!platform::schedule::exists(r->name)) {
                // Deleted from the OS's own tools: the record goes too.
                removeRecord(r->name);
                continue;
            }
            out.push_back(*r);
        }
    }
    std::sort(out.begin(), out.end(), [](const ScheduledLaunch& a, const ScheduledLaunch& b) {
        return a.whenMs != b.whenMs ? a.whenMs < b.whenMs : a.id < b.id;
    });
    return out;
}

void completeScheduledLaunch(const std::string& name) {
    if (name.empty() || sanitize(name, 200) != name) return;
    std::optional<ScheduledLaunch> r = readRecord(recordDir() / (name + ".json"));
    std::string why;
    if (!platform::schedule::remove(name, &why)) LOG_WARN("scheduled launch %s: %s", name.c_str(), why.c_str());
    removeRecord(name);
    if (!r) {
        r = ScheduledLaunch{};
        r->name = name;
    }
    LOG_INFO("Started by the scheduled launch '%s' (%s)", r->id.c_str(), name.c_str());
    g_launchedBy = std::move(r);
}

const std::optional<ScheduledLaunch>& launchedBySchedule() { return g_launchedBy; }

}  // namespace bro::engine
