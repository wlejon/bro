#pragma once

#include "embed/embed.h"

namespace bro::engine { class Engine; }

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// Install the interpreter bridge hooks.
void installInterpBridge(engine::Engine& engine);

// The value under `name` as something compiled code can use, or undefined if not set.
Value bridgeJsGlobal(const char* name);

// Reclaim inactive bridge entries.
void sweepInterpBridge();

// Drop all bridge entries.
void resetInterpBridge();

}  // namespace bro::bronze_host
