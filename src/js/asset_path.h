#pragma once

#include <quickjs.h>

#include <string>

namespace bro::util { class AssetMounts; }

namespace bro::js {

/// One place where JS-facing bindings turn an app-supplied path string into a
/// real filesystem path, so every binding accepts the same spellings:
///
///   "C:\\..." / "D:/..."   drive-qualified — passed through untouched
///   "/lib/foo.png"          leading slash — resolved against engine mounts
///   "sounds/hit.ogg"        anything else — relative to the app directory
///
/// This used to be a private static copy in each of image/tile/scene/diffusion
/// bindings, which is exactly why the audio file APIs — the one surface that
/// never got a copy — silently rejected "/app/..." paths that work everywhere
/// else. New bindings should call this instead of touching paths themselves.
///
/// Two binding files deliberately keep their own copy and must NOT be folded
/// in here, because for them the context is not process-wide:
///   - image_bindings installs once per document, and sub-documents and system
///     panels install it again with THEIR base path and no mounts (see
///     sub_document.cpp / system_panels.cpp). Routing that through this shared
///     context lets the last panel loaded clobber the app's mounts.
///   - diffusion_bindings uses thread_local state because it also runs in
///     worker realms.
///
/// Main-thread control plane only: setAssetPathContext runs once during engine
/// init, before any JS executes, and the context is read-only after.
void setAssetPathContext(const std::string& basePath, const util::AssetMounts* mounts);

std::string resolveAssetPath(const std::string& src);

/// Install `bro.appDir` and `bro.resolvePath(path)` on the JS global.
///
/// Reading an app's own files already works through the `/app` mount, but a
/// mount path is virtual: it cannot be handed to an external process, and it
/// cannot be used to launch a binary the app ships alongside itself. Nothing
/// else fills that gap — `process.cwd()` is wherever the user launched bro
/// from, not the app directory, and `__dirname` only exists inside require()'d
/// modules. So an app that bundles a sidecar executable or hands a path to a
/// command-line tool had no way to name it.
///
/// resolvePath applies exactly the rules above and returns a real filesystem
/// path; appDir is the app's own root, the value most callers actually want.
///
/// Also `bro.userDataDir`: the per-user, per-app directory an app's saves and
/// caches belong in (util/user_dirs.h) — the app directory is where an app is
/// installed, not where it writes. Created on first read; empty with no app.
void installAssetPathBindings(JSContext* ctx);

/// The same rules, for a path being WRITTEN to. A write target does not exist
/// yet, so asking a mount to resolve the file itself can't work; this resolves
/// the parent directory and rejoins the filename. Use it for every save/export
/// binding — the read/write asymmetry is why the audio file APIs accepted
/// "/app/..." on load and silently wrote next to the executable on save.
std::string resolveAssetWritePath(const std::string& src);

} // namespace bro::js
