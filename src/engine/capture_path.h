#pragma once

// Shared by the headless capture APIs (screenshot, screenshotCanvas): make the
// destination writable before handing the path to the PNG encoder.
//
// These APIs exist to dump artifacts from automated runs, and a test that writes
// its frames to `tests/out/` should not fail because nothing has created
// `tests/out/` yet — the encoder just gets a fopen failure and the binding
// reports a bare "screenshot failed", which says nothing about the cause and
// costs a debugging session to work out. Creating the parent directory is what
// every screenshot tool does; the app should not have to.

#include <filesystem>
#include <string>

namespace bro {

/// Create `path`'s parent directory if it is missing. Returns false only when
/// the directory does not exist and cannot be created — a caller that gets
/// false should report the path, since that is the part the user can fix.
/// A path with no parent component (a bare filename) is trivially fine.
inline bool ensureParentDir(const std::string& path) {
    std::error_code ec;
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) return true;
    if (std::filesystem::exists(parent, ec)) return true;
    std::filesystem::create_directories(parent, ec);
    return !ec;
}

}  // namespace bro
