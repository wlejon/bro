#pragma once

#include <string>

namespace bro::bronze_host {

// Synchronizes the current realm's registered host globals
// (`ev::registerGlobal`) with any replaceable properties a script has
// overwritten on its `globalThis`, so bare global reads in subsequent scripts,
// `-e`, or `bro-ctl eval` see the new binding.
void syncHostGlobalsFromGlobalThis();

// Whether `name` is one of the host's own globals (the app's realm as
// installed, or registered since): what the source preflight treats as known.
bool isBaselineHostGlobalName(const std::string& name);

// The app realm's host globals, as installed (installWebHostGlobals), and
// put back at a reload over any the app replaced.
void snapshotBaselineHostGlobalNames();
void restoreBaselineHostGlobals();

}  // namespace bro::bronze_host
