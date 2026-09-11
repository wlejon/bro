#pragma once

#include <string>

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

/// Compile JS code in-process using bronze CLI and run it on `engine`.
bool evalScript(engine::Engine& engine, const std::string& code,
                const std::string& filename = "<eval>");

/// Compile a JS file in-process using bronze CLI and run it on `engine`.
bool evalScriptFile(engine::Engine& engine, const std::string& filePath);

/// Resolve path to web_host.globals manifest.
std::string getWebHostGlobalsPath();

} // namespace bro::bronze_host
