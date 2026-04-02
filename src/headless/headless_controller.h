#pragma once

#include <string>

namespace bro::engine { class Engine; }

namespace bro::headless {

/// Thin command controller that drives the unified Engine in headless mode.
/// Provides text-based commands for automated testing and debugging.
class HeadlessController {
public:
    explicit HeadlessController(engine::Engine& engine);

    /// Process a single command line. Returns false on "quit"/"exit".
    bool processCommand(const std::string& line);

    /// Run an interactive command loop reading from stdin.
    void runInteractive();

    /// Run a script file with commands.
    void runScript(const std::string& path);

private:
    engine::Engine& engine_;
    std::string lastDump_;
};

} // namespace bro::headless
