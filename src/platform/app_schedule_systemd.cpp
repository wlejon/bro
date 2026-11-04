// app_schedule.h on Linux: a systemd user timer and the service it starts,
// as unit files under ~/.config/systemd/user (so they outlive a reboot),
// loaded, enabled and started over the session bus (brodbus):
//
//   <name>.service   Type=exec, ExecStart=<command>, the desktop session's
//                    display variables captured at scheduling time
//   <name>.timer     OnCalendar=<UTC time>, AccuracySec=1s, Persistent=true
//                    (a launch missed while the machine was off runs at
//                    boot), WakeSystem= from `wake`
#include "platform/app_schedule.h"

#include "brodbus/bus.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>

namespace bro::platform::schedule {

namespace {

namespace fs = std::filesystem;

constexpr const char* kDest = "org.freedesktop.systemd1";
constexpr const char* kPath = "/org/freedesktop/systemd1";
constexpr const char* kManager = "org.freedesktop.systemd1.Manager";

fs::path unitDir() {
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return fs::path(x) / "systemd" / "user";
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".config" / "systemd" / "user";
}

// One argument as systemd's ExecStart reads it.
std::string execQuote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '%') out += '%';
        if (c == '$') out += '$';
        out += c;
    }
    return out + "\"";
}

std::string envQuote(const std::string& kv) {
    std::string out = "\"";
    for (char c : kv) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '%') out += '%';
        out += c;
    }
    return out + "\"";
}

std::string utcCalendar(int64_t whenMs) {
    std::time_t t = static_cast<std::time_t>((whenMs + 999) / 1000);
    std::tm g{};
    gmtime_r(&t, &g);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d UTC", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                  g.tm_hour, g.tm_min, g.tm_sec);
    return buf;
}

std::unique_ptr<brodbus::Bus> userBus(std::string& why) {
    std::string err;
    auto bus = brodbus::Bus::open_user(&err);
    if (!bus) why = "cannot reach the systemd user manager over the session bus: " + err;
    return bus;
}

bool callManager(brodbus::Bus& bus, const char* member, const std::function<void(brodbus::Message&)>& args,
                 std::string& why) {
    std::string err;
    if (bus.call_method(kDest, kPath, kManager, member, args, nullptr, &err)) return true;
    why = std::string("systemd ") + member + ": " + err;
    return false;
}

// The unit as the user manager read it: false (with why) unless it loaded,
// since a unit file systemd rejects only shows in the journal otherwise.
bool unitLoaded(brodbus::Bus& bus, const std::string& unit, std::string& why) {
    std::string path, err;
    if (!bus.call_method(kDest, kPath, kManager, "LoadUnit", [&](brodbus::Message& m) { m.append_string(unit); },
                         [&](brodbus::Message& r) { r.read_object_path(&path); }, &err) ||
        path.empty()) {
        why = "systemd LoadUnit " + unit + ": " + err;
        return false;
    }
    std::string state;
    if (!bus.get_property_string(kDest, path, "org.freedesktop.systemd1.Unit", "LoadState", &state, &err)) {
        why = "systemd " + unit + " LoadState: " + err;
        return false;
    }
    if (state != "loaded") {
        why = "systemd did not load " + unit + " (" + state + "; journalctl --user names the line)";
        return false;
    }
    return true;
}

// A path as a unit file setting reads it: verbatim, specifiers escaped.
std::string unitPath(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '%') out += '%';
        out += c;
    }
    return out;
}

}  // namespace

std::string backend(std::string* why) {
    std::string err;
    if (!userBus(err)) {
        if (why) *why = err;
        return {};
    }
    return "systemd";
}

bool add(const Launch& launch, std::string& why) {
    if (launch.command.empty()) {
        why = "no command";
        return false;
    }
    auto bus = userBus(why);
    if (!bus) return false;
    std::error_code ec;
    const fs::path dir = unitDir();
    fs::create_directories(dir, ec);
    const std::string service = launch.name + ".service", timer = launch.name + ".timer";
    {
        std::ofstream f(dir / service, std::ios::trunc);
        f << "# Written by bro (bro.app.schedule); removed when it runs or is unscheduled.\n"
          << "[Unit]\nDescription=" << unitPath(launch.description) << "\n\n[Service]\nType=exec\n";
        if (!launch.cwd.empty()) f << "WorkingDirectory=" << unitPath(launch.cwd) << "\n";
        // The desktop session's display, as the app would have it launched
        // from the session (the user manager's own environment may lack it).
        for (const char* var : {"WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR", "XDG_SESSION_TYPE",
                                "XDG_CURRENT_DESKTOP", "DBUS_SESSION_BUS_ADDRESS", "BRO_APP_HOME"}) {
            if (const char* v = std::getenv(var); v && *v) f << "Environment=" << envQuote(std::string(var) + "=" + v) << "\n";
        }
        f << "ExecStart=";
        for (size_t i = 0; i < launch.command.size(); ++i) f << (i ? " " : "") << execQuote(launch.command[i]);
        f << "\n";
        if (!f) {
            why = "writing " + (dir / service).string() + " failed";
            return false;
        }
    }
    {
        std::ofstream f(dir / timer, std::ios::trunc);
        f << "# Written by bro (bro.app.schedule); removed when it runs or is unscheduled.\n"
          << "[Unit]\nDescription=" << unitPath(launch.description) << "\n\n[Timer]\nOnCalendar=" << utcCalendar(launch.whenMs)
          << "\nAccuracySec=1s\nPersistent=true\nWakeSystem=" << (launch.wake ? "true" : "false")
          << "\nUnit=" << service << "\n\n[Install]\nWantedBy=timers.target\n";
        if (!f) {
            why = "writing " + (dir / timer).string() + " failed";
            return false;
        }
    }
    if (!callManager(*bus, "Reload", nullptr, why)) return false;
    if (!unitLoaded(*bus, service, why) || !unitLoaded(*bus, timer, why)) {
        fs::remove(dir / service, ec);
        fs::remove(dir / timer, ec);
        std::string ignored;
        callManager(*bus, "Reload", nullptr, ignored);
        return false;
    }
    // Enabled: started again at the next login (after a reboot); started now
    // for this session.
    if (!callManager(*bus, "EnableUnitFiles",
                     [&](brodbus::Message& m) {
                         m.append_string_list({timer});
                         m.append_bool(false);
                         m.append_bool(true);
                     },
                     why))
        return false;
    // A timer already started keeps its old time; restart it to read the new one.
    return callManager(*bus, "RestartUnit",
                       [&](brodbus::Message& m) {
                           m.append_string(timer);
                           m.append_string("replace");
                       },
                       why);
}

bool remove(const std::string& name, std::string* why) {
    const fs::path dir = unitDir();
    const std::string service = name + ".service", timer = name + ".timer";
    std::error_code ec;
    const bool had = fs::exists(dir / timer, ec) || fs::exists(dir / service, ec);
    std::string err;
    auto bus = userBus(err);
    if (bus && had) {
        std::string ignored;
        callManager(*bus, "StopUnit",
                    [&](brodbus::Message& m) {
                        m.append_string(timer);
                        m.append_string("replace");
                    },
                    ignored);
        callManager(*bus, "DisableUnitFiles",
                    [&](brodbus::Message& m) {
                        m.append_string_list({timer});
                        m.append_bool(false);
                    },
                    ignored);
    }
    fs::remove(dir / timer, ec);
    fs::remove(dir / service, ec);
    if (bus && had) {
        std::string ignored;
        callManager(*bus, "Reload", nullptr, ignored);
    }
    const bool gone = !fs::exists(dir / timer, ec) && !fs::exists(dir / service, ec);
    if (!gone && why) *why = "could not remove the units of " + name;
    return gone;
}

bool exists(const std::string& name) {
    std::error_code ec;
    return fs::exists(unitDir() / (name + ".timer"), ec);
}

}  // namespace bro::platform::schedule
