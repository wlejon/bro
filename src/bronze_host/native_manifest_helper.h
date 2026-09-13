#pragma once

#include <string>

namespace bro::bronze_host {

/// Resolves the directory containing C-ABI native manifest JSON files.
/// Probes environment variables (BRO_NATIVE_MANIFEST, BRONZE_NATIVE_MANIFEST)
/// and candidate directory locations relative to the executable, project root, and CWD.
/// Returns the directory path if it exists and contains .json files, or empty string.
std::string getNativeManifestDir();

/// Resolves the path to the native C-ABI shared library
/// (libbro_c_abi.so, bro_c_abi.dll, or libbro_c_abi.dylib).
/// Probes environment variables (BRO_NATIVE_LIB, BRONZE_NATIVE_LIB) and candidate
/// library locations in the build directory, executable directory, or beside libbronze_runtime_shared.
/// Returns the library path if found, or empty string.
std::string getNativeLibPath();

} // namespace bro::bronze_host
