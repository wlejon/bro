#pragma once

#include <string>
#include <vector>

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

/// Register headless host globals (advanceTime, flush, sleep, screenshot, assert,
/// click, mouseDown, mouseUp, mouseMove, wheel, keyDown, keyUp, textInput, scriptArgs,
/// gamepadConnect, gamepadDisconnect, gamepadButton, gamepadAxis)
/// on `engine`.
void installHeadlessGlobals(engine::Engine& engine);

/// Set command-line script arguments accessible via `scriptArgs` in JS.
void setScriptArgs(const std::vector<std::string>& args);

/// Test failure tracking for headless assertions.
bool hasTestFailure();
void clearTestFailure();
void setTestFailure(bool failed = true);

/// Set by the script's skipTest(reason) / missingGpuContext(kind): the run
/// could not test its subject in this environment. A run that skipped and did
/// not also fail exits 77 (the automake SKIP convention run_tests.sh reads).
bool wasTestSkipped();

} // namespace bro::bronze_host
