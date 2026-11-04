#pragma once

#include "embed/embed.h"

namespace bro::bronze_host {

/// bro.window.captureDisplays and the desktop capture behind
/// bro.window.captureScreen(options) (host_screen_capture.cpp).
void installBroWindowScreenCapture(bronze::Value broWin);

}  // namespace bro::bronze_host
