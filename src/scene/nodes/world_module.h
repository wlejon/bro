#pragma once
// A world's module: GLSL a node is
// given, compiled into its kind's shaders at run time. Nodes key their
// compiled shader sets by its source's hash.

#include <cstdint>
#include <string>

namespace bro::scene {

/// FNV-1a of a module's source: 0 for none, never 0 for one.
inline uint64_t moduleSourceHash(const std::string& source) {
    if (source.empty()) return 0;
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : source) h = (h ^ c) * 1099511628211ull;
    return h ? h : 1;
}

}  // namespace bro::scene
