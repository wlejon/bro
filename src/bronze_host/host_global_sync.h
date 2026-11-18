#pragma once

#include <string>

namespace bro::bronze_host {

// Whether `name` is one of the host's own globals (the app's realm as
// installed, or registered since): what the source preflight treats as known.
bool isBaselineHostGlobalName(const std::string& name);

// The app realm's host globals, as installed (installWebHostGlobals), and
// put back at a reload, with the kinds they were installed with, over any the
// app replaced, redefined or deleted.
void snapshotBaselineHostGlobalNames();
void restoreBaselineHostGlobals();

}  // namespace bro::bronze_host
