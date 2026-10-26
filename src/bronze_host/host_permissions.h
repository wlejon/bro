#pragma once

// The app's capabilities in the bronze host (host_permissions.cpp; the
// authority itself is engine/app_permissions.h).

#include "embed/embed.h"

namespace bro::bronze_host {

using Value = bronze::Value;

/// brokit's access policy (fs, child_process, the network) and the
/// terminal's spawn gate, over the app's permissions. Process-wide, once,
/// before any realm runs a script.
void installAccessPolicy();

/// The privileged namespaces on `bro` (and bro.sys's members) made live, the
/// unavailable stub, or the stub that asks on first touch, by what the app is
/// granted; and bro.permissions for the shell. Main realm, after the
/// siblings' namespaces are in.
void installGatedNamespaces(Value bro);

/// Whether bro.<name> is the stub that asks on first touch (a declared,
/// undecided capability): nothing may read it on the app's behalf.
bool isAskingNamespace(const char* name);

}  // namespace bro::bronze_host
