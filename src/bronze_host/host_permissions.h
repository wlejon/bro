#pragma once

// The app's capabilities in the bronze host (host_permissions.cpp; the
// authority itself is engine/app_permissions.h).

#include "embed/embed.h"

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

using Value = bronze::Value;

/// bronze's access gate (every library's files, processes, sockets,
/// captures, environment and desktop configuration), the terminal's spawn
/// gate and broaudio's engine-level capture gate, over the app's
/// permissions. Process-wide, once, before any realm runs a script.
void installAccessPolicy();

/// The grant store's watch (a change from another process is read within a
/// quarter second and what it took back revoked, held resources included),
/// the main thread's revalidation of what it holds open, the background
/// verification of an installed app's content, and the last-use record.
void installPermissionsPump(engine::Engine& engine);

/// The privileged namespaces on `bro` (and bro.sys's members): live when the
/// host grants them, else a proxy answering from the capability's state at
/// every touch (live, asking, or the unavailable stub); and bro.permissions
/// for the shell. Main realm, after the siblings' namespaces are in.
void installGatedNamespaces(Value bro);

/// Whether bro.<name> is a gated proxy (its capability is not the host's):
/// nothing may read it on the app's behalf, since a read is a use that may
/// ask the user.
bool isGatedNamespace(const char* name);

}  // namespace bro::bronze_host
