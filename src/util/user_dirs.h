#pragma once

#include <string>

namespace bro::util {

/// The per-user directory bro itself owns: `%APPDATA%\bro` on Windows,
/// `~/Library/Application Support/bro` on macOS, `$XDG_DATA_HOME/bro` (or
/// `~/.local/share/bro`) elsewhere. The projects registry, the remote-asset
/// cache and every app's own data live under it. Never empty — a machine with
/// no home directory at all gets `.bro` relative to the working directory,
/// which is wrong but writable.
std::string userDataDir();

/// The per-user directory for data bro can always rebuild — the compiled-code
/// cache — and so the one place that should neither roam with a Windows
/// profile nor be backed up: `%LOCALAPPDATA%\bro` on Windows,
/// `~/Library/Caches/bro` on macOS, `$XDG_CACHE_HOME/bro` (or `~/.cache/bro`)
/// elsewhere. Falls back to userDataDir() when none of those is known.
std::string userCacheDir();

} // namespace bro::util
