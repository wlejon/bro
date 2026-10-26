#include "engine/capabilities.h"

namespace bro::engine {

namespace {

using K = CapabilityKind;

// The one table. A namespace capability's name is its namespace's
// (capabilityForNamespace leans on that); a bro.sys member's is sys.<member>.
constexpr Capability kCatalogue[] = {
    // --- asked for ---
    {"files", K::Ask, "read and write files anywhere in your home folder"},
    {"net", K::Ask, "use the network"},
    {"spawn", K::Ask, "start programs and terminals"},
    {"mic", K::Ask, "record from the microphone"},
    {"wl", K::Ask, "use Wayland desktop protocols (panels, window lists, screen capture)"},
    {"displays", K::Ask, "change display configuration and brightness"},
    {"clip", K::Ask, "read the clipboard history"},
    {"pulse", K::Ask, "change audio routing and volumes"},
    {"remote", K::Ask, "share this screen with a remote viewer and take its input"},
    {"sys.power", K::Ask, "read power state and suspend, reboot or power off"},
    {"sys.audio", K::Ask, "change system audio devices and volumes"},
    {"sys.network", K::Ask, "change network connections"},
    {"sys.bluetooth", K::Ask, "pair and connect Bluetooth devices"},
    {"sys.notifications", K::Ask, "act as the desktop's notification server"},
    {"sys.tray", K::Ask, "act as the desktop's system tray"},
    {"sys.processes", K::Ask, "list and signal processes"},
    // --- the host's alone ---
    {"shell", K::Shell, "be the desktop shell (own the compositor's input and frames)"},
    {"compositor", K::Shell, "manage every window on the desktop"},
    {"seat", K::Shell, "control the session, switch terminals and lock the screen"},
    {"cred", K::Shell, "authenticate the user and answer credential prompts"},
    {"portal", K::Shell, "answer other apps' file, screenshot and screencast requests"},
    {"permissions", K::Shell, "view and change what every app is granted"},
};

constexpr const char* kSysMembers[] = {
    "power", "audio", "network", "bluetooth", "notifications", "tray", "processes",
};

}  // namespace

std::span<const Capability> capabilityCatalogue() { return kCatalogue; }

const Capability* findCapability(std::string_view name) {
    for (const Capability& c : kCatalogue)
        if (name == c.name) return &c;
    return nullptr;
}

std::string capabilityForNamespace(std::string_view ns) {
    // Only the capabilities that ARE a namespace: `shell` is a role, and
    // files, net, spawn and mic are exercised through many surfaces (checked
    // where each is exercised), never as one bro.<name> object. bro.mic is
    // one of mic's surfaces, gated at the capture itself.
    if (ns == "shell" || ns == "files" || ns == "net" || ns == "spawn" || ns == "mic") return {};
    const Capability* c = findCapability(ns);
    return c ? std::string(c->name) : std::string();
}

std::span<const char* const> sysCapabilityMembers() { return kSysMembers; }

void appendGrantList(std::string_view list, std::vector<std::string>& out) {
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = list.find(',', start);
        std::string_view item = list.substr(start, comma == std::string_view::npos ? std::string_view::npos
                                                                                   : comma - start);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (!item.empty()) out.emplace_back(item);
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
}

std::string capabilityNameList() {
    std::string out;
    for (const Capability& c : kCatalogue) {
        if (!out.empty()) out += ", ";
        out += c.name;
    }
    return out;
}

std::vector<std::string> allCapabilityNames() {
    std::vector<std::string> out;
    for (const Capability& c : kCatalogue) out.emplace_back(c.name);
    return out;
}

}  // namespace bro::engine
