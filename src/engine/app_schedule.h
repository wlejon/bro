#pragma once

// bro.app.schedule's records (docs/app-api.js, docs/apps.md "Scheduled
// launches"): a launch of an app the OS starts at a time
// (platform/app_schedule.h), one record per launch beside it in bro's data
// directory, `<userDataDir>/schedule/<name>.json` (`<BRO_APP_HOME>/schedule`
// in a scratch profile). The OS entry starts
//
//   <executable> --scheduled <name> [<app folder>] <args...>
//
// and the run it starts deletes the entry and the record before anything else
// (completeScheduledLaunch), so each launch happens once.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bro::engine {

struct AppRuntimeInfo;

struct ScheduledLaunch {
    std::string name;                  // the OS's name for it
    std::string id;                    // the app's
    int64_t whenMs = 0;
    std::vector<std::string> args;     // what the launch hands the app
    std::vector<std::string> command;  // the whole command line the OS runs
    bool wake = false;
    std::string appDir;
};

/// Whether launches are recorded only (bro-headless: like bro.app.open, a
/// test's schedules reach no OS unless BRO_SCHEDULE_OS=1), else real.
void setScheduleRecordOnly(bool recordOnly);
bool scheduleRecordOnly();

/// The OS backend's name ("task-scheduler", "systemd", "launchd", or
/// "recorded" for a record-only run); "" with `why` where there is none.
std::string scheduleBackend(std::string* why = nullptr);

/// Schedule (or move) the app's launch `id` at `whenMs`. `launcher` is the
/// command that starts the app (its executable, or bro and its folder);
/// `--scheduled <name>` goes right after its executable, `args` after it
/// all. False with `why`.
bool scheduleAppLaunch(const AppRuntimeInfo& app, const std::string& id, int64_t whenMs,
                       const std::vector<std::string>& args, bool wake, const std::vector<std::string>& launcher,
                       ScheduledLaunch& out, std::string& why);

/// Remove the app's launch `id`; whether there was one.
bool unscheduleAppLaunch(const AppRuntimeInfo& app, const std::string& id, std::string* why = nullptr);

/// The app's pending launches, soonest first. Records whose OS entry is
/// gone (removed by hand) are dropped.
std::vector<ScheduledLaunch> scheduledAppLaunches(const AppRuntimeInfo& app);

/// `--scheduled <name>`: this run is that launch. Removes its OS entry and
/// record and remembers it for launchedBySchedule(). Call from main before
/// the single-instance hand-off.
void completeScheduledLaunch(const std::string& name);

/// The launch that started this run, if a schedule did.
const std::optional<ScheduledLaunch>& launchedBySchedule();

/// Whether `id` is a valid launch id: 1-64 of [A-Za-z0-9._-].
bool isValidScheduleId(const std::string& id);

}  // namespace bro::engine
