#pragma once

#include <cstdint>

namespace bro::platform::desktop {

/// Triggers system bell sound (MessageBeep on Windows, terminal bell/canberra on Linux).
/// Returns true if bell was triggered or recorded.
bool beep();

/// Headless test inspection.
uint32_t getHeadlessBeepCount();
void resetHeadlessBeepCount();

} // namespace bro::platform::desktop
