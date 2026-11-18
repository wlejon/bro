#pragma once

#include <string>

namespace bro::util {

class AssetMounts;

/// Sets the base path and asset mounts context for resolving paths.
void setAssetPathContext(const std::string& basePath, const AssetMounts* mounts);

/// Resolves an asset path string into a filesystem path:
///   "C:\\..." / "D:/..."   drive-qualified — passed through untouched
///   "/lib/foo.png"          leading slash — resolved against engine mounts
///   "sounds/hit.ogg"        anything else — relative to the app directory
std::string resolveAssetPath(const std::string& src);

/// Resolves a path being written to:
/// Resolves the parent directory against mounts/basePath and rejoins the filename.
std::string resolveAssetWritePath(const std::string& src);

/// The app folder `bro://app/` names (the engine sets it as it loads the app).
void setAppUrlRoot(const std::string& appDir);
/// The file a `bro://app/<path>` URL names (percent-decoded, query and
/// fragment dropped), or "" for any other URL or a path outside the folder.
std::string pathForAppUrl(const std::string& url);
// The absolute path a `bro://app/` URL spells ("/a/b.png" for
// "bro://app/a/b.png?v=1"), "" for another URL. An absolute path attribute
// (`img.src = "/tmp/x.jpg"`, a mount or a filesystem path: resolveAssetPath)
// reflects as such a URL, and this is the way back.
std::string appUrlPath(const std::string& url);

} // namespace bro::util
