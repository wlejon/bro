#pragma once

#include <string>

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

/// Compile JS code in-process using bronze CLI and run it on `engine`.
/// `filename` is the document the code came from (an app's index.html for its
/// `<script>` text): its relative imports resolve from there. Empty means
/// "no document", and they resolve from the app dir instead.
bool evalScript(engine::Engine& engine, const std::string& code,
                const std::string& filename = {});

/// Compile a JS file in-process using bronze CLI and run it on `engine`.
bool evalScriptFile(engine::Engine& engine, const std::string& filePath);

/// Resolve path to web_host.globals manifest.
std::string getWebHostGlobalsPath();

/// Install dynamic evaluation and function hooks (eval(), new Function())
/// into the Bronze runtime.
void installDynamicHooks(engine::Engine& engine);

} // namespace bro::bronze_host
