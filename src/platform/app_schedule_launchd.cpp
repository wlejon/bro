// app_schedule.h on macOS: a launchd user agent, ~/Library/LaunchAgents/
// <name>.plist (loaded again at every login, so it outlives a reboot),
// booted into the user's GUI domain with launchctl. Its
// StartCalendarInterval is local time in whole minutes, so the launch comes
// at the first minute boundary at or after `when`; launchd runs one missed
// while the machine slept when it wakes (it cannot wake the machine itself).
#include "platform/app_schedule.h"

#include "util/process_spawn.h"

#include <unistd.h>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace bro::platform::schedule {

namespace {

namespace fs = std::filesystem;

fs::path agentDir() {
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / "Library" / "LaunchAgents";
}

fs::path plistPath(const std::string& name) { return agentDir() / (name + ".plist"); }

std::string xml(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string guiDomain() { return "gui/" + std::to_string(getuid()); }

int launchctl(const std::vector<std::string>& args, std::string& why) {
    std::vector<std::string> argv{"/bin/launchctl"};
    argv.insert(argv.end(), args.begin(), args.end());
    std::string err;
    const fs::path log = fs::temp_directory_path() / ("bro-launchctl-" + std::to_string(getpid()) + ".txt");
    const int rc = util::runAndWait(argv, err, log.string());
    std::error_code ec;
    if (rc != 0) {
        std::ifstream in(log);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
        why = "launchctl " + args[0] + " failed (" + std::to_string(rc) + ")" + (text.empty() ? err : ": " + text);
    }
    fs::remove(log, ec);
    return rc;
}

}  // namespace

std::string backend(std::string*) { return "launchd"; }

bool add(const Launch& launch, std::string& why) {
    if (launch.command.empty()) {
        why = "no command";
        return false;
    }
    std::error_code ec;
    fs::create_directories(agentDir(), ec);
    // The first whole minute at or after `when`, local time.
    std::time_t t = static_cast<std::time_t>((launch.whenMs + 59999) / 60000 * 60);
    std::tm lt{};
    localtime_r(&t, &lt);
    const fs::path path = plistPath(launch.name);
    {
        std::ofstream f(path, std::ios::trunc);
        f << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<!-- Written by bro (bro.app.schedule); removed when it runs or is unscheduled. -->\n"
             "<plist version=\"1.0\">\n<dict>\n"
          << "  <key>Label</key><string>" << xml(launch.name) << "</string>\n"
          << "  <key>ProgramArguments</key>\n  <array>\n";
        for (const std::string& a : launch.command) f << "    <string>" << xml(a) << "</string>\n";
        f << "  </array>\n";
        if (!launch.cwd.empty()) f << "  <key>WorkingDirectory</key><string>" << xml(launch.cwd) << "</string>\n";
        if (const char* home = std::getenv("BRO_APP_HOME"); home && *home)
            f << "  <key>EnvironmentVariables</key>\n  <dict><key>BRO_APP_HOME</key><string>" << xml(home)
              << "</string></dict>\n";
        f << "  <key>LimitLoadToSessionType</key><string>Aqua</string>\n"
          << "  <key>StartCalendarInterval</key>\n  <dict>\n"
          << "    <key>Month</key><integer>" << lt.tm_mon + 1 << "</integer>\n"
          << "    <key>Day</key><integer>" << lt.tm_mday << "</integer>\n"
          << "    <key>Hour</key><integer>" << lt.tm_hour << "</integer>\n"
          << "    <key>Minute</key><integer>" << lt.tm_min << "</integer>\n"
          << "  </dict>\n</dict>\n</plist>\n";
        if (!f) {
            why = "writing " + path.string() + " failed";
            return false;
        }
    }
    // Replace one already loaded under this label.
    std::string ignored;
    launchctl({"bootout", guiDomain() + "/" + launch.name}, ignored);
    if (launchctl({"bootstrap", guiDomain(), path.string()}, why) != 0) {
        fs::remove(path, ec);
        why += " (the GUI domain exists only while the user is logged in at the console)";
        return false;
    }
    return true;
}

bool remove(const std::string& name, std::string* why) {
    std::error_code ec;
    const fs::path path = plistPath(name);
    std::string ignored;
    if (fs::exists(path, ec)) launchctl({"bootout", guiDomain() + "/" + name}, ignored);
    fs::remove(path, ec);
    const bool gone = !fs::exists(path, ec);
    if (!gone && why) *why = "could not remove " + path.string();
    return gone;
}

bool exists(const std::string& name) {
    std::error_code ec;
    return fs::exists(plistPath(name), ec);
}

}  // namespace bro::platform::schedule
