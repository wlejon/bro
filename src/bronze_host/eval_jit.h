#pragma once

#include <string>

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

/// Checks whether in-memory JIT evaluation is disabled via environment variables
/// (e.g. BRO_DISABLE_JIT=1, BRO_EVAL_AOT=1, BRO_NO_JIT=1).
bool isJitDisabled();

/// Evaluates JavaScript code in-memory using the Brass JIT engine without disk files.
/// Returns true on success, false on failure (and logs error / sets test failure).
bool evalScriptJit(engine::Engine& engine, const std::string& code, const std::string& filename = {});

/// Evaluates a JavaScript file in-memory using the Brass JIT engine without disk files.
/// Returns true on success, false on failure (and logs error / sets test failure).
bool evalScriptFileJit(engine::Engine& engine, const std::string& filePath);

} // namespace bro::bronze_host
