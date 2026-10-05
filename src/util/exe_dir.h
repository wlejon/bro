#pragma once

#include <string>

namespace bro::util {

/// Absolute path of the directory containing the running executable. Falls
/// back to "." if the platform query fails.
std::string executableDir();

/// Where the read-only data shipped with bro lives — `system/`, the bundled
/// Vulkan driver manifest. That is `Bro.app/Contents/Resources` when the
/// executable runs from a macOS app bundle (code signing wants code in
/// Contents/MacOS and data in Contents/Resources), and executableDir()
/// everywhere else, bro-headless beside the bundle included.
std::string resourceDir();

/// True when the executable runs from a macOS app bundle's Contents/MacOS.
bool inAppBundle();

/// The default `.bro_settings.json`: beside the executable, so a portable
/// install keeps its settings with it — except inside an app bundle, whose
/// contents its signature seals, where it is the one in userDataDir().
std::string defaultSettingsPath();

} // namespace bro::util
