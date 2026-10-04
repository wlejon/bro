#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bro::util {

/// Subprocess execution output and exit status.
struct SubprocessResult {
    int exitCode = -1;
    bool success = false;
    std::vector<uint8_t> stdOut;
    std::string stdErr;
};

/// Check if an executable exists on PATH or at the specified path.
/// Safe and portable across Windows, macOS, and Linux without shell execution.
bool hasExecutableOnPath(const std::string& name);

/// Execute a subprocess portably with stdin input, capturing stdout and stderr.
/// Uses posix_spawnp without raw fork hazards on POSIX, and CreateProcessA on Windows.
SubprocessResult runSubprocess(const std::vector<std::string>& args,
                              const std::string& inputStdin = "");

} // namespace bro::util
