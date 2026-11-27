#pragma once

#include <string>

namespace bro::bronze_host {

/// Configure the dedicated script-log output path (`--script-log <path>`).
/// Empty disables script-log capture.
void setScriptLogPath(const std::string& path);

/// Returns true if a non-empty `--script-log` path has been configured.
bool hasScriptLogPath();

/// Append a single formatted line to the script log file under a mutex.
void writeScriptLogLine(const std::string& level, const std::string& line);

/// Wrap `console.{log,info,warn,error,debug}` in the current JS realm to tee
/// formatted messages to the script log while preserving original behavior.
/// No-op if `--script-log` is unset; idempotent per realm.
void installScriptLogSink();

}  // namespace bro::bronze_host
