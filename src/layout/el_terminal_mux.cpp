// ElTerminal's persistent sessions: attach / detach to a bromux server's
// session (terminal/term_session.h, terminal/term_mux.h), the session list,
// and letting go when the element leaves the document. A terminal that
// spawns or attaches again after its process exited or it detached gets a
// fresh TermSession carrying the element's options and theme.

#include "layout/el_terminal_impl.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/shadow_root.h"
#include "terminal/term_cwd.h"
#include "terminal/term_mux.h"

#include <algorithm>

namespace bro::layout {

namespace {

// Connected in the DOM sense: a parent chain (through shadow roots to their
// hosts) up to the document element.
bool connected(dom::Node* n) {
    while (n) {
        if (n->nodeType() == dom::NodeType::DocumentFragment) {
            auto* sr = dynamic_cast<dom::ShadowRoot*>(n);
            n = sr ? sr->host() : nullptr;
            continue;
        }
        dom::Node* parent = n->parentNode();
        if (!parent) {
            dom::Document* doc = n->document();
            return doc && n->nodeType() == dom::NodeType::Element && doc->documentElement() == n;
        }
        n = parent;
    }
    return false;
}

} // namespace

bool ElTerminal::readySession(bool attaching, std::string* error) {
    Impl& m = *impl_;
    if (!m.session->spawned()) return true;
    if (m.session->running()) {
        if (error) *error = "this terminal already has a process";
        return false;
    }
    // spawn() is once per element, except after a detach (the element then
    // holds nothing); attach() takes over from anything finished.
    if (!attaching && !m.session->detached()) {
        if (error) *error = "a process already ran in this terminal";
        return false;
    }
    // Used: start over with a fresh session, sized as the grid is now.
    m.session = std::make_unique<terminal::TermSession>(std::max(1, m.lastCols ? m.lastCols : 80),
                                                        std::max(1, m.lastRows ? m.lastRows : 24));
    m.exitDispatched = false;
    m.detachDispatched = false;
    m.haveView = false;
    m.activityParsed = m.activityRemote = 0;  // the new session counts from zero
    m.paletteKey.clear();      // the theme goes to the new session ...
    m.imageMemoryLimit = -1.0;  // ... and so does every option
    setOptions(options_);
    return true;
}

void ElTerminal::detachIfRemoved() {
    terminal::TermSession& s = *impl_->session;
    if (!s.persistent() || s.detached() || s.exited()) return;
    if (!connected(elem_)) s.detach();
}

bool ElTerminal::attach(uint64_t sessionId, const std::string& server, std::string* error) {
    if (!readySession(/*attaching=*/true, error)) return false;
    return impl_->session->attach(sessionId, server, error);
}

void ElTerminal::detach() { impl_->session->detach(); }

uint64_t ElTerminal::sessionId() const {
    const terminal::TermSession& s = *impl_->session;
    return s.persistent() ? s.sessionId() : 0;
}

bool ElTerminal::persistentAvailable() { return terminal::muxAvailable(); }

std::optional<std::vector<ElTerminal::SessionInfo>> ElTerminal::sessions(const std::string& server,
                                                                      std::string* error) {
    auto list = terminal::muxListSessions(server, error);
    if (!list) return std::nullopt;
    std::vector<SessionInfo> out;
    out.reserve(list->size());
    for (const terminal::MuxSessionInfo& s : *list) {
        SessionInfo i;
        i.id = s.id;
        i.name = s.name;
        i.command = s.command;
        i.title = s.title;
        i.cwd = terminal::cwdFromUri(s.cwd).path;  // OSC 7's URI, as the element's `cwd` reads it
        i.pid = s.pid;
        i.running = s.running;
        i.exitCode = s.exitCode;
        i.cols = s.cols;
        i.rows = s.rows;
        i.clients = s.clients;
        i.createdMs = double(s.createdMs);
        out.push_back(std::move(i));
    }
    return out;
}

bool ElTerminal::closeSession(const std::string& server, uint64_t id, std::string* error) {
    return terminal::muxCloseSession(server, id, error);
}

bool ElTerminal::killServer(const std::string& server, std::string* error) {
    return terminal::muxKillServer(server, error);
}

} // namespace bro::layout
