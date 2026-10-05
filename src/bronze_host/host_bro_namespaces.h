#pragma once

// The host-built members of the `bro` root (host_bro_root.cpp assembles
// them): bro.math, bro.text, bro.gpu, bro.menu, bro.steam, and the stand-in
// for a namespace compiled out of this build. bro.media is host_media.h.

#include "embed/embed.h"

#include <string>

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// Math (host_math_classes.cpp / host_math_funcs.cpp)
Value makeBroMathValue();
void installMathGlobals();

// Text (host_text.cpp)
Value makeBroTextValue();

// bro.gpu (host_gpu.cpp): the runtime backend probe over brotensor/CPU fallback.
Value makeBroGpuValue();

// Menu (host_menu.cpp)
Value makeBroMenuValue();

// Steam (host_steam.cpp)
Value makeBroSteamValue();
void drainSteamEvents();
void cleanupSteamBindings();


// Stubs for unavailable / compiled-out subsystems (host_bro_root.cpp)
Value makeUnavailableNamespace(const std::string& name, const std::string& flag);

}  // namespace bro::bronze_host
