#pragma once

// A cheap consistency check of an app's source tree, run before a
// watcher-triggered reload tears the running page down.
//
// The source watcher fires once the tree has been quiet for a moment, but
// quiet is not consistent: a git checkout, a rebase or a build step that
// copies a UI into place can pause mid-way, and a reload that starts then
// compiles half of the old tree against half of the new one. Once the old
// page is torn down there is nothing to go back to, so the check runs first:
// it reads index.html, the scripts it names, and the static
// import graph of its module scripts, and answers whether everything they
// reference is there — every file, and every named import exported by the
// file it names. It is a heuristic over source text (no parse), written to
// err on the side of "consistent": an unrecognised export form counts as
// present, a bare specifier is not followed.

#include <string>

namespace bro::util { class AssetMounts; }

namespace bro::engine {

/// Empty when the app at `appDir` looks complete enough to reload; otherwise
/// one line saying what is missing (`js/shell.js imports NotificationCenter
/// from js/notify.js, which does not export it`).
std::string checkAppSourceConsistency(const std::string& appDir,
                                      const util::AssetMounts* mounts = nullptr);

} // namespace bro::engine
