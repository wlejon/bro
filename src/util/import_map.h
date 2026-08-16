#pragma once

#include <string>
#include <utility>
#include <vector>

namespace bro::util {

/// A WHATWG import map — the `<script type="importmap">` a page declares to
/// give bare module specifiers ("three", "three/addons/loaders/RGBELoader.js")
/// a meaning. Without one a bare specifier has no resolution at all, which is
/// why an unmodified three.js app cannot load: `import * as THREE from 'three'`
/// names a package, not a file.
///
/// Only the top-level `imports` object is honoured. `scopes` is recognised and
/// then *reported*, not silently ignored: a scope exists to hand a different
/// module to importers under one path prefix, so resolving those the global way
/// would load a file the page did not ask for — a wrong module is worse than a
/// missing one, and the warning is the difference.
class ImportMap {
public:
    /// Parse an import-map JSON document. `baseDir` is the directory the map's
    /// relative targets resolve against — the document's base URL on the web,
    /// the app dir here.
    ///
    /// Returns false and leaves the map untouched when the JSON does not parse.
    /// A half-applied map is worse than none: it would resolve some specifiers
    /// and fail others, and the failures would look like missing files.
    bool parse(const std::string& json, const std::string& baseDir);

    /// Resolve a specifier against the map. Returns an empty string when the
    /// map says nothing about it — the caller's signal to fall through to its
    /// own resolution, not to fail.
    std::string resolve(const std::string& specifier) const;

    bool empty() const { return exact_.empty() && prefix_.empty(); }

    /// Number of entries, for logging what a page actually declared.
    size_t size() const { return exact_.size() + prefix_.size(); }

private:
    /// Keys that must match a specifier in full ("three").
    std::vector<std::pair<std::string, std::string>> exact_;
    /// Keys ending in '/', matched as prefixes ("three/addons/"). Held sorted
    /// longest-key-first, so the spec's most-specific-wins rule falls out of
    /// the first hit of a linear scan.
    std::vector<std::pair<std::string, std::string>> prefix_;
};

} // namespace bro::util
