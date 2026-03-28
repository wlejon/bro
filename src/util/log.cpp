#include "util/log.h"

// The logging implementation is entirely inline in the header.
// This translation unit exists to satisfy the CMake source list
// and to provide a place for future non-inline logging features
// (e.g., file output, log filtering by level).
