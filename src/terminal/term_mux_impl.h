#pragma once
// TermSession's persistent-session state (private to src/terminal, with
// bromux): what term_session_mux.cpp and term_session_mux_state.cpp share.
// Every member is used under the session's mu_.

#include "terminal/term_mux_source.h"
#include "terminal/term_session.h"

#include <bromux/client.h>
#include <bromux/screen_model.h>

#include <bropty/view.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace bro::terminal {

struct TermSession::Mux {
    std::unique_ptr<bromux::Client> client;  // destroyed last: the views read its mirror
    uint64_t id = 0;
    bool attached = false;
    std::unique_ptr<MuxSource> source;
    std::unique_ptr<bropty::TerminalView> view;
    std::vector<bromux::ClientEvent> events;
    uint64_t feedSeq = 0;  // the server's state version last seen (remoteUpdates_)
};

} // namespace bro::terminal
