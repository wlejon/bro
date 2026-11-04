#pragma once

// Launches the OS starts at a time, with bro not running: what
// bro.app.schedule rests on (engine/app_schedule.h keeps the records; this
// is only the OS's half).
//
//   Windows  Task Scheduler (ITaskService): a per-user task in the root
//            folder, interactive token, no elevation
//   Linux    a systemd user timer + service under ~/.config/systemd/user,
//            enabled and started over D-Bus (brodbus)
//   macOS    a launchd user agent under ~/Library/LaunchAgents, booted into
//            the GUI domain; launchd's calendar is in whole minutes
//
// Each survives a reboot, and a launch missed while the machine was off or
// asleep runs when it is back.

#include <cstdint>
#include <string>
#include <vector>

namespace bro::platform::schedule {

struct Launch {
    std::string name;                  // unique, [A-Za-z0-9._-]: the task / unit / label name
    int64_t whenMs = 0;                // Unix time, ms
    std::vector<std::string> command;  // argv[0] absolute
    std::string cwd;
    bool wake = false;                 // wake the machine from sleep for it (Windows, Linux)
    std::string description;
};

/// "task-scheduler" | "systemd" | "launchd", or "" where there is none
/// (`why` says so).
std::string backend(std::string* why = nullptr);

/// Create (or replace) the launch. False with `why`.
bool add(const Launch& launch, std::string& why);

/// Delete it; true when it is gone afterwards (absent counts).
bool remove(const std::string& name, std::string* why = nullptr);

/// Whether the OS still holds it.
bool exists(const std::string& name);

}  // namespace bro::platform::schedule
