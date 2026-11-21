#pragma once

#include "embed/embed.h"

namespace bro::bronze_host {

using Value = bronze::Value;

/// Builds `bro.eye`. `hasEngine` is false in Worker realms where `audit()` and
/// `scene()` are unavailable (while `measure`, `compare`, `visualize`, `fit`
/// over images / ImageData / ImageBitmap / file paths work identically).
Value makeBroEyeValue(bool hasEngine = true);

}  // namespace bro::bronze_host
