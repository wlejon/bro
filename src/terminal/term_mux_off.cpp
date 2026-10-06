// Persistent sessions compiled out (bro built without bromux): the calls
// fail and say why; a TermSession is only ever local.
#include "terminal/term_mux.h"
#include "terminal/term_session.h"

namespace bro::terminal {

namespace {
constexpr const char* kOff = "persistent sessions are not built in (bro was built without bromux)";
}

bool muxAvailable() { return false; }

std::optional<std::vector<MuxSessionInfo>> muxListSessions(const std::string&, std::string* error) {
    if (error) *error = kOff;
    return std::nullopt;
}

bool muxCloseSession(const std::string&, uint64_t, std::string* error) {
    if (error) *error = kOff;
    return false;
}

bool muxKillServer(const std::string&, std::string* error) {
    if (error) *error = kOff;
    return false;
}

struct TermSession::Mux {};
void TermSession::MuxDelete::operator()(Mux* m) const { delete m; }

bool TermSession::spawnPersistent(const SpawnOptions&, const PersistentOptions&, std::string* error) {
    if (error) *error = kOff;
    return false;
}

bool TermSession::attach(uint64_t, const std::string&, std::string* error) {
    if (error) *error = kOff;
    return false;
}

void TermSession::detach() {}

// Never reached: mux_ is always null here.
bool TermSession::muxPump(std::chrono::steady_clock::time_point, bool& published) {
    published = false;
    return false;
}
bool TermSession::muxAttachLocked(uint64_t, std::string*) { return false; }
void TermSession::muxDropView() {}
void TermSession::muxMirrorLost() {}
void TermSession::muxKill() {}
bool TermSession::muxWrite(std::string_view) { return false; }
bool TermSession::muxSendKey(const bropty::KeyEvent&) { return false; }
bool TermSession::muxSendText(std::string_view, bool) { return false; }
bool TermSession::muxFocus(bool) { return false; }
bool TermSession::muxMouse(const bropty::MouseEvent&) { return false; }
void TermSession::muxResize(int, int, int, int) {}
bool TermSession::muxAnswerClipboard(uint64_t, bool, std::string_view) { return false; }
void TermSession::muxSetBasePalette(const bropty::Palette&) {}
std::string TermSession::muxTitle() const { return {}; }
std::string TermSession::muxCwd() const { return {}; }
uint32_t TermSession::muxKittyFlags() const { return 0; }
std::string TermSession::muxScrollbackText() const { return {}; }

} // namespace bro::terminal
