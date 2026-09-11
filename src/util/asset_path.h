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

} // namespace bro::util
