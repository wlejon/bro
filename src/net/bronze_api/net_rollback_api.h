#pragma once

#include "embed/embed.h"

namespace bro::bronze_host {

// Creates a rollback netcode session JS object from (opts) or (tickFn, opts),
// running the determinism lint on the tick function and enabling runtime
// state-hash desync detection.
bronze::Value createRollbackSessionValue(std::span<const bronze::Value> args);

// Verified P2P code capsule JS bindings (`bro.net.createVerifiedCapsule`,
// `bro.net.verifyCapsule`, `bro.net.loadVerifiedCapsule`).
bronze::Value createVerifiedCapsuleValue(std::span<const bronze::Value> args);
bronze::Value verifyCapsuleValue(std::span<const bronze::Value> args);
bronze::Value loadVerifiedCapsuleValue(std::span<const bronze::Value> args);

// Attaches `createRollbackSession` and capsule verification helpers onto `bro.net`.
void installNetRollbackExtensions();

}  // namespace bro::bronze_host
