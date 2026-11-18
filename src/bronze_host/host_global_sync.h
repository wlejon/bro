#pragma once

#include <string>

namespace bro::bronze_host {

// Makes the current realm's host globals that a script replaced on its global
// object (a [Replaceable] one, an operation, an interface object) answer the
// replacement to bare reads too, registering it with the name's kind; the
// accessor globals are the global object's already. Run after each script.
//
// Only until bronze reads a replaced host global from the global object on
// its own (embed.h, GlobalKind): its write watch (rtNoteGlobalObjectWrite)
// misses a global object whose prototype the host set, as bro sets the
// window's (host_global_proto.cpp), so the replacement never reaches a bare
// read without this.
void syncHostGlobalsFromGlobalThis();

// Whether `name` is one of the host's own globals (the app's realm as
// installed, or registered since): what the source preflight treats as known.
bool isBaselineHostGlobalName(const std::string& name);

// The app realm's host globals, as installed (installWebHostGlobals), and
// put back at a reload, with the kinds they were installed with, over any the
// app replaced, redefined or deleted.
void snapshotBaselineHostGlobalNames();
void restoreBaselineHostGlobals();

}  // namespace bro::bronze_host
