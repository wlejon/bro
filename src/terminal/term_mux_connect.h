#pragma once
// How bro connects to a bromux server (private to src/terminal).

#include <bromux/client.h>

#include <string>

namespace bro::terminal {

// Named server (empty: the default), started on demand when `autostart`.
bromux::ConnectOptions muxConnectOptions(const std::string& server, bool autostart);

} // namespace bro::terminal
