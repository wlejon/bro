// ElTerminal's parked sessions: a session handed over by park() outlives its
// element (and the page: the table is process-wide, untouched by a reload),
// for a <terminal> of the next page to adopt().
//
// Nothing in a TermSession points back at an element or a document: its
// parser thread publishes frames into its own channel and wakes the main loop
// through util::wakeMainLoop, its Host queues the program's events in the
// session, and the layer (TermLayer, the engine's TerminalLayers entry) is
// the element's, keyed by the element's own layer id. So a session moves
// between elements by moving the unique_ptr: the adopter's pump takes the
// frames and the queued events from then on, and its own layer paints them.
// The events the program raised that the old element had not dispatched yet
// stay queued in the session and go to the adopter.

#include "layout/el_terminal_impl.h"

#include "util/log.h"

#include <algorithm>
#include <vector>

namespace bro::layout {

namespace {

struct Parked {
    std::string key;
    std::unique_ptr<terminal::TermSession> session;
    // What the parking element had reported as `activity`, so the adopter
    // reports the output parsed while the session was parked, not all of it.
    uint64_t activityParsed = 0, activityRemote = 0;
    // The grace period (ElTerminal::sweepParked): frames swept, and the
    // clock at the first sweep (-1: not swept since parked / the reload).
    int frames = 0;
    double sinceMs = -1.0;
};

std::vector<Parked>& parkedTable() {
    static std::vector<Parked> t;
    return t;
}

std::vector<Parked>::iterator findParked(const std::string& key) {
    auto& t = parkedTable();
    return std::find_if(t.begin(), t.end(), [&](const Parked& p) { return p.key == key; });
}

// The binding puts the app's identity and a newline before the page's own
// key (host_element_terminal.cpp, parkKey): messages name the page's.
std::string shownKey(const std::string& key) {
    const size_t nl = key.rfind('\n');
    return nl == std::string::npos ? key : key.substr(nl + 1);
}

} // namespace

bool ElTerminal::park(const std::string& key, std::string* error) {
    Impl& m = *impl_;
    if (key.empty()) {
        if (error) *error = "the key is empty";
        return false;
    }
    if (!m.session->spawned() || m.session->detached()) {
        if (error) *error = "this terminal has no session to park";
        return false;
    }
    if (findParked(key) != parkedTable().end()) {
        if (error) *error = "a session is already parked as '" + shownKey(key) + "'";
        return false;
    }
    // A key held for its text goes to the program now, and a program that
    // asked for focus reports hears that nothing has focus while parked (the
    // adopter's pump reports its own).
    flushDeferredKey();
    if (m.focused) m.session->focus(false);
    Parked p;
    p.key = key;
    p.session = std::move(m.session);
    p.activityParsed = m.activityParsed;
    p.activityRemote = m.activityRemote;
    parkedTable().push_back(std::move(p));
    // The element holds nothing now, as after a detach: a fresh session.
    m.session = std::make_unique<terminal::TermSession>(std::max(1, m.lastCols ? m.lastCols : 80),
                                                        std::max(1, m.lastRows ? m.lastRows : 24));
    resetForNewSession();
    return true;
}

bool ElTerminal::adopt(const std::string& key, std::string* error) {
    Impl& m = *impl_;
    auto it = findParked(key);
    if (it == parkedTable().end()) return false;
    if (m.session->running()) {
        if (error) *error = "this terminal already has a process";
        return false;
    }
    // Typing into a running shell is as much as starting one (as attach).
    if (std::string refused = terminal::checkSpawnGate("take over the terminal session parked as '" +
                                                          shownKey(key) + "'");
        !refused.empty()) {
        if (error) *error = refused;
        return false;
    }
    std::unique_ptr<terminal::TermSession> session = std::move(it->session);
    const uint64_t parsed = it->activityParsed, remote = it->activityRemote;
    parkedTable().erase(it);
    m.session = std::move(session);
    resetForNewSession();
    m.activityParsed = parsed;
    m.activityRemote = remote;
    // To this element's grid (the caller lays the document out first); the
    // first pump then takes the newest frame, whatever was published while
    // parked included.
    fitToBox();
    return true;
}

void ElTerminal::sweepParked(double nowMs) {
    auto& t = parkedTable();
    if (t.empty()) return;
    std::vector<std::unique_ptr<terminal::TermSession>> ended;
    for (auto it = t.begin(); it != t.end();) {
        if (it->sinceMs < 0.0) it->sinceMs = nowMs;
        ++it->frames;
        if (it->frames > kParkGraceFrames && nowMs - it->sinceMs >= kParkGraceMs) {
            LOG_INFO("The terminal session parked as '%s' was not adopted: ending it", shownKey(it->key).c_str());
            ended.push_back(std::move(it->session));
            it = t.erase(it);
        } else {
            ++it;
        }
    }
    // Destroyed outside the walk: ~TermSession stops the child (bounded, but
    // it may wait out bropty's grace periods).
    ended.clear();
}

void ElTerminal::restartParkedGrace() {
    for (Parked& p : parkedTable()) {
        p.frames = 0;
        p.sinceMs = -1.0;
    }
}

void ElTerminal::dropParked() {
    // Moved out first: a destructor never runs while the table is mid-change.
    std::vector<Parked> all = std::move(parkedTable());
    parkedTable().clear();
    all.clear();
}

size_t ElTerminal::parkedCount() { return parkedTable().size(); }

} // namespace bro::layout
