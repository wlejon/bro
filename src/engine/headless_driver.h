#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bro::engine {

class Engine;

struct HeadlessHooks {
    std::function<void()> beforeEngine;

    /// Run once the Engine is up and its first run() has returned — virtual
    /// time rebased, the app's document laid out — and before the headless
    /// globals go in or any script evaluates: the seam a host with native
    /// logic of its own starts it at, where a page's own JS already is by
    /// this point (finished, with its first frame scheduled). The driver
    /// script that follows then steps a running app rather than starting one.
    std::function<void(Engine&)> afterEngine;

    /// Host bindings to register when bronze web host globals are installed.
    std::function<void(Engine&)> installHostBindings;

    /// Host-extension APIs mounted onto `bro` inside installBroRoots before permission gating.
    std::function<void(Engine&)> installHostRootApis;

    /// Per-thread host bindings for each Worker (EngineConfig::installWorkerHostBindings).
    std::function<void()> installWorkerHostBindings;

    /// Called when a child compartment is destroyed.
    std::function<void(uint64_t)> onDropCompartment;

    /// Called during hot-reload beside shutdownSiblingApisForReload().
    std::function<void()> onShutdownForReload;

    /// Run after the Engine is destroyed and immediately before `_exit()`.
    ///
    /// The driver leaves through `_exit()`, which skips `atexit` handlers and
    /// static destruction on purpose (see the teardown comment in
    /// headless_driver.cpp). Anything a host wants reported at the end of a
    /// run — a profile table, a leak census — has to be called here rather
    /// than registered with the CRT, because the CRT never gets the chance.
    std::function<void()> beforeExit;

    /// Capabilities this host executable grants its app outright, before any
    /// `--grant` (docs/apps.md, Permissions): a shell's own headless driver
    /// grants its UI what the windowed shell does. Empty for bro-headless.
    std::vector<std::string> hostGrants;

    /// Name used in usage text and diagnostics.
    std::string programName = "bro-headless";

    /// One-line summary shown at the top of --help.
    std::string tagline = "headless mode for bro";
};

/// The headless driver: argument parsing, engine construction, script/REPL
/// evaluation and teardown. Returns the process exit code.
int runHeadless(int argc, char* argv[], const HeadlessHooks& hooks = {});

} // namespace bro::engine
