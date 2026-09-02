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

/// The directory an app should keep its own persistent data in — saves,
/// caches, per-user state — `<userDataDir()>/apps/<name>`, where `<name>` is
/// the app directory's folder name reduced to `[A-Za-z0-9._-]`. Empty when
/// `appDir` is empty (a bare `bro-headless -e` session has no app).
///
/// The alternative, and what localStorage does, is to write next to
/// `index.html`. That works for a project checked out from git and fails for
/// every other way an app reaches a user: an install directory the user cannot
/// write to, a read-only zip mount, a shared app folder with several users on
/// the machine. Nothing is created here; the caller decides when a directory
/// should come into existence (the JS binding creates it on first read).
std::string appUserDataDir(const std::string& appDir);

} // namespace bro::util
