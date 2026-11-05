#pragma once

// Crash reports for an app with a log of its own (docs/apps.md, "Crash
// reports"). A run that crashes leaves its log ending in the crash handler's
// report (util/crash_handler.h), and on Windows a minidump beside it; a run
// killed outright leaves a log that never reached its clean-exit line. The
// next launch keeps that log (and dump) before truncating the log for
// itself, and sends what was kept to the URL the app's bro.json names, from
// a background thread. A report the receiver has not accepted (no network,
// receiver down) waits in the crashes directory for a later launch.

#include <string>

namespace bro::util {

// Who and what crashed, sent with each report.
struct CrashReportInfo {
    std::string url;          // where to POST; "" sends nothing (reports are still kept)
    std::string appId;
    std::string appName;
    std::string appVersion;
    std::string engineVersion;
};

// The line a run writes to its log as it returns from main: a log without it
// was not closed cleanly.
inline constexpr const char* kCleanExitLine = "bro: clean exit";

// Before the launch truncates `logFile`: when the previous run's log holds a
// crash report, or is not empty and lacks kCleanExitLine, move it (and the
// minidump at `dumpFile`, if one is there) into <logDir>/crashes/ as
// <stem>-<time>.log / .dmp. A leftover dump with no crash is deleted.
// Returns the kept log's path, "" when there was nothing to keep.
std::string keepCrashedLog(const std::string& logFile, const std::string& dumpFile);

// Write kCleanExitLine to stderr (the log).
void noteCleanExit();

// Send every report in <logDir>/crashes/ not yet accepted, on a detached
// thread. An accepted report is renamed *.sent; the newest few sent ones are
// kept for a look, older ones deleted.
void sendCrashReports(const std::string& logDir, const CrashReportInfo& info);

// The same, on the calling thread: how many reports the receiver accepted.
// It stops at the first one it does not (the rest wait for a later launch).
int sendPendingCrashReports(const std::string& logDir, const CrashReportInfo& info);

} // namespace bro::util
