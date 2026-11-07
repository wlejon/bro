#pragma once

// What a brokernel source says about itself, read without compiling it: the
// modules it imports and the functions it defines. A scan of its tokens
// (identifiers, `::` paths, punctuation), with comments and string literals
// skipped, in one pass over the text.

#include <string>
#include <string_view>
#include <vector>

namespace bro::scene {

/// The modules `source` names in its imports, in the order they appear:
///   import a::b;            import a::b as c;     -> a::b
///   from a::b import x;                           -> a::b
///   use a::b::{x, y};       use a::b::*;          -> a::b
///   use a::b::x;            use a::b::x as y;     -> a::b
std::vector<std::string> kernelSourceImports(std::string_view source);

/// Whether `source` defines `fn <name>(` (any generic-free definition).
bool kernelSourceDefinesFn(std::string_view source, std::string_view name);

}  // namespace bro::scene
