// <terminal> compiled out (BRO_WITH_TERMINAL off): an inert box of the
// default size. No session, no layer; every call does nothing.

#include "layout/el_terminal.h"

#include <algorithm>

namespace bro::layout {

std::vector<ElTerminal*>& termRegistry();
uint64_t termNextLayerId();
int termIntAttr(const dom::Element* el, const char* name, int fallback, int lo, int hi);

struct ElTerminal::Impl {};

bool ElTerminal::available() { return false; }
ElTerminal::ElTerminal(render::Renderer* renderer) : renderer_(renderer), layerId_(termNextLayerId()) {
    termRegistry().push_back(this);
}
ElTerminal::~ElTerminal() {
    auto& r = termRegistry();
    r.erase(std::remove(r.begin(), r.end(), this), r.end());
}
void ElTerminal::refreshFont() {}
void ElTerminal::refreshTheme() {}
void ElTerminal::dispatchEvents() {}
void ElTerminal::dispatchActivity() {}
void ElTerminal::autoScroll(double) {}
void ElTerminal::getContentSize(float& w, float& h) {
    w = float(termIntAttr(elem_, "cols", 80, 1, 1000)) * 8.0f;
    h = float(termIntAttr(elem_, "rows", 24, 1, 1000)) * 16.0f;
}
std::shared_ptr<TermLayer> ElTerminal::layer() const { return nullptr; }
bool ElTerminal::recordLayer(float) { return false; }
uint64_t ElTerminal::layerRecords() const { return 0; }
uint64_t ElTerminal::totalLayerRecords() { return 0; }
void ElTerminal::draw(render::Renderer*, float, float, float, float) {}
bool ElTerminal::spawn(const SpawnSpec&, std::string* error) {
    if (error) *error = "<terminal> is not compiled into this build (BRO_WITH_TERMINAL)";
    return false;
}
bool ElTerminal::attach(uint64_t, const std::string&, std::string* error) {
    if (error) *error = "<terminal> is not compiled into this build (BRO_WITH_TERMINAL)";
    return false;
}
void ElTerminal::detach() {}
uint64_t ElTerminal::sessionId() const { return 0; }
bool ElTerminal::persistentAvailable() { return false; }
std::optional<std::vector<ElTerminal::SessionInfo>> ElTerminal::sessions(const std::string&, std::string* error) {
    if (error) *error = "<terminal> is not compiled into this build (BRO_WITH_TERMINAL)";
    return std::nullopt;
}
bool ElTerminal::closeSession(const std::string&, uint64_t, std::string*) { return false; }
bool ElTerminal::killServer(const std::string&, std::string*) { return false; }
bool ElTerminal::readySession(bool, std::string*) { return false; }
void ElTerminal::detachIfRemoved() {}
bool ElTerminal::write(std::string_view) { return false; }
void ElTerminal::feed(std::string_view) {}
void ElTerminal::kill() {}
int64_t ElTerminal::pid() const { return 0; }
double ElTerminal::lastInputMs() const { return 0.0; }
double ElTerminal::shownOutputMs() const { return 0.0; }
double ElTerminal::pendingOutputMs() const { return 0.0; }
bool ElTerminal::running() const { return false; }
bool ElTerminal::exited() const { return false; }
std::optional<int> ElTerminal::exitCode() const { return std::nullopt; }
std::optional<ElTerminal::ProcessInfo> ElTerminal::foregroundProcess() const { return std::nullopt; }
int ElTerminal::cols() const { return 0; }
int ElTerminal::rows() const { return 0; }
std::string ElTerminal::screenText() const { return {}; }
std::string ElTerminal::scrollbackText() const { return {}; }
std::string ElTerminal::frameText() const { return {}; }
std::string ElTerminal::title() const { return {}; }
std::string ElTerminal::cwd() const { return {}; }
std::string ElTerminal::cwdUri() const { return {}; }
bool ElTerminal::bracketedPaste() const { return false; }
std::string ElTerminal::selectionText() const { return {}; }
std::string ElTerminal::defaultShell() { return {}; }
ElTerminal::CursorInfo ElTerminal::cursor() const { return {}; }
ElTerminal::Metrics ElTerminal::metrics() const { return {}; }
void ElTerminal::flushDeferredKey() {}
bool ElTerminal::keyDown(int, int, int, bool) { return false; }
bool ElTerminal::keyUp(int, int, int) { return false; }
void ElTerminal::keyCancelled() {}
bool ElTerminal::textInput(std::string_view) { return false; }
void ElTerminal::setPreedit(std::string_view) {}
bool ElTerminal::paste(std::string_view) { return false; }
bool ElTerminal::caretRect(float&, float&, float&, float&) const { return false; }
ElTerminal::MouseResult ElTerminal::mouseDown(float, float, int, int, int) { return {}; }
ElTerminal::MouseResult ElTerminal::mouseMove(float, float, int) { return {}; }
ElTerminal::MouseResult ElTerminal::mouseUp(float, float, int, int) { return {}; }
bool ElTerminal::wheel(float, float, float, int) { return false; }
void ElTerminal::mouseLeave() {}
std::string ElTerminal::pointerCursor(int) const { return "default"; }
bool ElTerminal::takeCursorChanged() { return false; }
ElTerminal::ViewInfo ElTerminal::viewInfo() const { return {}; }
bool ElTerminal::scroll(ScrollOp, int64_t) { return false; }
void ElTerminal::selectRange(const Range&) {}
void ElTerminal::selectAll() {}
void ElTerminal::clearSelection() {}
bool ElTerminal::selectOutput(std::optional<std::pair<int64_t, int>>) { return false; }
std::optional<ElTerminal::Range> ElTerminal::selectionRange() const { return std::nullopt; }
bool ElTerminal::search(const SearchQuery&, std::string* error) {
    if (error) *error = "<terminal> is not compiled into this build (BRO_WITH_TERMINAL)";
    return false;
}
void ElTerminal::clearSearch() {}
std::optional<ElTerminal::Range> ElTerminal::searchNext(bool) { return std::nullopt; }
ElTerminal::SearchInfo ElTerminal::searchInfo() const { return {}; }
std::optional<ElTerminal::Link> ElTerminal::linkAt(int64_t, int) const { return std::nullopt; }
std::string ElTerminal::rangeText(const Range&) const { return {}; }
std::vector<ElTerminal::Command> ElTerminal::commands() const { return {}; }
ElTerminal::ImageInfo ElTerminal::images() const { return {}; }
bool ElTerminal::answerClipboard(uint64_t, std::string_view) { return false; }
bool ElTerminal::denyClipboard(uint64_t) { return false; }
std::string ElTerminal::pointerShape() const { return {}; }
void ElTerminal::setOptions(const Options& o) { options_ = o; }
ElTerminal::Theme ElTerminal::theme() const { return {}; }
ElTerminal::Theme ElTerminal::palette() const { return {}; }
void ElTerminal::setTheme(const Theme& t) { scriptTheme_ = t; }
bool ElTerminal::pump(double, bool, float) { return false; }

} // namespace bro::layout
