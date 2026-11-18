#pragma once

#include <string>

namespace bro::bronze_host {

// The app realm's host globals, as installed (installWebHostGlobals), and
// put back at a reload, with the kinds they were installed with, over any the
// app replaced, redefined or deleted.
void snapshotBaselineHostGlobals();
void restoreBaselineHostGlobals();

}  // namespace bro::bronze_host
