#pragma once

// The capability catalogue: every power an app can be granted beyond its own
// page and its own directories, in one table that every check reads
// (docs/apps.md, Permissions). Nothing else in bro
// names a capability that is not here, so the manifest's validation, the
// prompts, the grant store, `bro --permissions` and the checks at the natives
// cannot disagree about what exists.
//
// Two kinds:
//   Ask    the user decides, and the decision is kept per app identity
//          (grant_store.h). An app may only ask for what its manifest's
//          `permissions` lists.
//   Shell  granted by the host executable alone (EngineConfig::hostGrants:
//          helm's main, `--grant` on bro and bro-headless), never by a prompt,
//          the store or dev mode. These are the desktop's own powers.
//
// What needs nothing is not here: the app's own directories, its DOM, canvas,
// WebGL and scene, audio output, timers, bro.conf, launching other apps
// through bro.apps / bro.app.open, and reading model weights.

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bro::engine {

enum class CapabilityKind { Ask, Shell };

struct Capability {
    const char* name;          // "net", "sys.processes"
    CapabilityKind kind;
    /// What granting it lets the app do, as the prompt and the listings say
    /// it: "use the network".
    const char* summary;
};

/// Every capability, in a stable order (the order listings print).
std::span<const Capability> capabilityCatalogue();

/// The entry named `name`, or null when there is none.
const Capability* findCapability(std::string_view name);

/// The capability a privileged `bro.<namespace>` member is reached through:
/// "wl" for bro.wl, "sys.processes" for bro.sys.processes. Empty when the
/// namespace is not gated (bro.sys itself is split by member; see
/// sysCapabilityMembers).
std::string capabilityForNamespace(std::string_view ns);

/// The members of bro.sys that are capabilities of their own ("power",
/// "processes", ...): sys.<member> each.
std::span<const char* const> sysCapabilityMembers();

/// Every capability's name, comma-separated: what an error naming an unknown
/// one lists.
std::string capabilityNameList();

/// Every capability's name: what a desktop shell's host executable grants
/// its own UI.
std::vector<std::string> allCapabilityNames();

/// `--grant`'s argument (`net,spawn`, `declared`) onto `out`, one name each.
/// Names are checked when the grants are applied (app_permissions.cpp).
void appendGrantList(std::string_view list, std::vector<std::string>& out);

}  // namespace bro::engine
