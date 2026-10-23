#pragma once

#include "embed/embed.h"

namespace bro::bronze_host {

// Creates a verified rollback netcode session JS object from (opts) or (tickFn, opts).
bronze::Value createRollbackSessionValue(std::span<const bronze::Value> args);

// Attaches `createRollbackSession` (and `verifyTickDeterminism`) onto `bro.net`.
void installNetRollbackExtensions();

}  // namespace bro::bronze_host
