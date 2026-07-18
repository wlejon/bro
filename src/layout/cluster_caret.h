#pragma once

#include <cstdlib>
#include <string_view>

// Whether caret geometry is answered from the shaper's cluster map or from
// prefix measurement.
//
// Prefix measurement — "the caret at byte N sits at width(text[0..N))" — is
// what every caret and selection surface in the engine used before shaping
// existed, and it is wrong wherever glyph advances are not independent: a
// kerned pair, a ligature, a joining script. Cluster consultation is right in
// all of those and identical in the case that dominates by volume, plain
// unkerned Latin, where the two agree to 0.000px.
//
// "Identical where it should be identical" is a claim about every editing
// surface at once, so it is worth being able to check rather than assert. This
// switch runs the engine both ways in one binary: BRO_CLUSTER_CARET=0 restores
// the prefix path exactly (the widened TextMetrics interface's own defaults),
// so a suite run, a screenshot or a caret x can be compared against the
// behaviour that shipped before, with no rebuild between the two.
//
// Default ON: the cluster path is the correct one, and the fallback exists to
// be checked against, not to be relied on.
namespace bro::layout {

inline bool clusterCaretEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("BRO_CLUSTER_CARET");
        return !(v && std::string_view(v) == "0");
    }();
    return enabled;
}

} // namespace bro::layout
