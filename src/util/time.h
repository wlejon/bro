#pragma once

namespace bro::util {

/// Returns the current time in milliseconds (monotonic clock).
/// Cross-platform: uses QueryPerformanceCounter on Windows, clock_gettime elsewhere.
double currentTimeMs();

} // namespace bro::util
