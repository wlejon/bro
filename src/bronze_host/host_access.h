#pragma once

// bro's own natives asking the host's access gate (embed/embed_access.h)
// before they read or write a path the app named, or connect where it said.
// The gate itself is installed by host_permissions.cpp (installAccessPolicy);
// this is only the asking side, the same for every native.
//
// A path is asked about made absolute and lexically normal (UTF-8); the host
// canonicalizes it. A path that came from bro itself (a drop, a pick, an
// app directory it chose) is not asked about.

#include "embed/embed_access.h"

#include <string>

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

/// `path` absolute and lexically normal, as the gate is asked about it.
std::string accessPath(const std::string& path);

/// The gate's answer for reading (`write` false) or writing `path`: "" when
/// allowed or when no gate is installed. *asked is the absolute path asked
/// about (left empty when no gate is installed).
std::string pathRefusal(const std::string& path, bool write, std::string* asked);

/// For a synchronous native: throws the access error (code EACCES, `path`)
/// into the running program when the gate refuses; returns when allowed.
void requirePathAccess(const std::string& path, bool write);

/// The same for a connection to `url` (AccessKind::Net): throws when refused.
void requireNetAccess(const std::string& url);

/// The same for starting `command` (AccessKind::Spawn): throws when refused.
void requireSpawnAccess(const std::string& command);

/// The test harness's own natives: the headless driver's writers and readers
/// (screenshot, screenshotCanvas, writeFile, controlCommand's output files
/// and `launch`, perf.loadShaderSet). Under bro-headless they are the
/// developer's harness driving the app, not the app's act, so they answer to
/// whoever ran the script and ask nothing of the app's grants; where they
/// also exist outside headless (a windowed bro installs them for its
/// self-tests) an app could call them, so there they ask the gate as any
/// native does.
void requireHarnessPathAccess(const engine::Engine& engine, const std::string& path, bool write);
void requireHarnessSpawnAccess(const engine::Engine& engine, const std::string& command);

/// An asset path the app named (util::resolveAssetPath: app-relative, a
/// mount, or absolute), resolved, after the gate allowed reading it; throws
/// when refused.
std::string readableAssetPath(const std::string& src);

}  // namespace bro::bronze_host
