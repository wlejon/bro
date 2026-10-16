#pragma once

// Starting other processes: a detached child (bro.app.spawn / open, an app
// executable's rebuild), and on Windows one whose exit status is waited for.
// POSIX exec in place is execv itself and needs no helper.

#include <string>
#include <vector>

namespace bro::util {

/// Start argv[0] with the rest as its arguments in `cwd` ("" keeps ours),
/// detached: on POSIX a grandchild in its own session (no zombie, survives
/// us); on Windows its own process group. False with `err` set on failure.
bool spawnDetached(const std::vector<std::string>& argv, const std::string& cwd, std::string& err);

/// Run argv to completion and return its exit status, -1 when it could not
/// be started (`err` set). Inherits our standard handles and working directory.
int runAndWait(const std::vector<std::string>& argv, std::string& err);

#ifdef _WIN32
/// The command line CreateProcessW wants for `argv`, each argument quoted the
/// way CommandLineToArgvW reads it back.
std::wstring windowsCommandLine(const std::vector<std::string>& argv);
#endif

}  // namespace bro::util
