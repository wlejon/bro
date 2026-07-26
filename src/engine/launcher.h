#pragma once

#include "engine/engine.h"

#include <string>

namespace bro::engine {

/// Launch plumbing shared by every executable that boots a bro Engine.
///
/// Resolving "what did the user point me at" is not trivial — a target can be
/// an app directory, a project directory, or a bro.json of either kind, and
/// the answer decides where /lib, /system, /std and /app mount. bro.exe,
/// bro-headless and bro-server each grew their own copy of the surrounding
/// helpers, and a host application that links bro_engine (ffmpeg-bro does)
/// would have had to grow a fourth. This is that logic, once.
///
/// What stays in each main() is only what genuinely differs: windowed run
/// loop vs headless script driver vs server tick loop.

/// Absolute path of the directory containing the running executable.
/// Falls back to "." if the platform query fails.
std::string executableDir();

/// Turn a possibly-relative path into an absolute one. Returns the input
/// unchanged when it cannot be resolved (e.g. it does not exist yet).
std::string absolutePath(const std::string& path);

/// Resolve the positional launch argument into `config.appDir` and
/// `config.projectRoot`, parsing whichever bro.json applies.
///
/// `target` may be empty, in which case the executable's own directory is
/// probed for a bro.json, an index.html, or the bundled project manager —
/// the behaviour of running `bro` with no arguments.
///
/// Returns false when no app could be located, which the caller should treat
/// as a usage error.
bool resolveLaunchTarget(const std::string& target, EngineConfig& config);

/// Publish the resolved locations to the environment: BRO_EXE_DIR,
/// BRO_APP_DIR and BRO_PROJECT_ROOT. JS reads these through process.env, and
/// child bro processes inherit them so a launcher's children land in the same
/// project. Call after resolveLaunchTarget; it also absolutises the paths in
/// `config` in place.
void publishLaunchEnv(EngineConfig& config);

} // namespace bro::engine
