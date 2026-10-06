#pragma once
// The working directory a shell reports with OSC 7, as a path.
//
// OSC 7 carries a URI: file://<host>/<percent-encoded path> by convention
// (bash / zsh integration scripts, fish, PowerShell, Windows Terminal's
// recipes), sometimes another scheme of the same shape (kitty's
// kitty-shell-cwd://<host>/<raw path>), and from some programs a bare path.
//
//   * file: the authority is the host ("" for file:///x, kept as written
//     otherwise, "localhost" included); the path is percent-decoded and ends
//     at a '?' or '#'.
//   * another scheme://host/path: host as above, the path as written.
//   * no "://": the text is the path already.
//
// A Windows drive path is returned in Windows form on every platform:
// file:///C:/Users/me (or /C|/Users/me) is C:\Users\me, file:///c: is C:\.
// Everything else keeps its slashes.

#include <string>
#include <string_view>

namespace bro::terminal {

struct CwdLocation {
    std::string path;  // "" when the URI names none
    std::string host;  // the URI's authority, "" for none
};

[[nodiscard]] CwdLocation cwdFromUri(std::string_view uri);

} // namespace bro::terminal
