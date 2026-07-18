#pragma once

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

} // namespace bro::js
