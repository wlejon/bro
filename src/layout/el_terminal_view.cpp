// ElTerminal's view calls: the scrollback position, selection, search,
// links, the shell's commands and the answers the page gives the program.
// Each forwards to the session, whose parser thread publishes the change
// with its next frame (and pump() then re-records the layer).

#include "layout/el_terminal_impl.h"

#include <algorithm>

namespace bro::layout {

ElTerminal::ViewInfo ElTerminal::viewInfo() const {
    const terminal::ViewState v = impl_->session->viewState();
    ViewInfo out;
    out.topRow = v.topRow;
    out.firstRow = v.firstRow;
    out.screenTopRow = v.screenTopRow;
    out.rows = v.rows;
    out.atBottom = v.atBottom;
    out.altScreen = v.altScreen;
    return out;
}

bool ElTerminal::scroll(ScrollOp op, int64_t amount) {
    terminal::TermSession& s = *impl_->session;
    const terminal::ViewState before = s.viewState();
    switch (op) {
        case ScrollOp::Lines: s.scrollBy(amount); break;
        case ScrollOp::Pages: s.scrollBy(amount * std::max(1, before.rows - 1)); break;
        case ScrollOp::Top: s.scrollToTop(); break;
        case ScrollOp::Bottom: s.scrollToBottom(); break;
        case ScrollOp::ToRow: s.scrollToRow(amount); break;
        case ScrollOp::PreviousPrompt: return s.scrollToPrompt(true);
        case ScrollOp::NextPrompt: return s.scrollToPrompt(false);
    }
    return s.viewState().topRow != before.topRow;
}

void ElTerminal::selectRange(const Range& r) { impl_->session->select(termToRowRange(r)); }
void ElTerminal::selectAll() { impl_->session->selectAll(); }
void ElTerminal::clearSelection() { impl_->session->selectClear(); }

bool ElTerminal::selectOutput(std::optional<std::pair<int64_t, int>> cell) {
    std::optional<bropty::RowPos> pos;
    if (cell) pos = bropty::RowPos{cell->first, cell->second};
    return impl_->session->selectOutput(pos);
}

std::optional<ElTerminal::Range> ElTerminal::selectionRange() const {
    const auto r = impl_->session->selectionRange();
    if (!r) return std::nullopt;
    return termFromRowRange(*r);
}

bool ElTerminal::search(const SearchQuery& q, std::string* error) {
    terminal::SearchOptions o;
    o.regex = q.regex;
    o.wholeWord = q.wholeWord;
    if (q.caseMode == "sensitive") o.caseMode = terminal::SearchOptions::Case::Sensitive;
    else if (q.caseMode == "insensitive") o.caseMode = terminal::SearchOptions::Case::Insensitive;
    else if (q.caseMode == "smart" || q.caseMode.empty()) o.caseMode = terminal::SearchOptions::Case::Smart;
    else {
        if (error) *error = "caseMode must be \"smart\", \"sensitive\" or \"insensitive\"";
        return false;
    }
    return impl_->session->searchStart(q.pattern, o, error);
}

void ElTerminal::clearSearch() { impl_->session->searchClear(); }

std::optional<ElTerminal::Range> ElTerminal::searchNext(bool backward) {
    const auto r = impl_->session->searchNext(backward);
    if (!r) return std::nullopt;
    return termFromRowRange(*r);
}

ElTerminal::SearchInfo ElTerminal::searchInfo() const {
    const terminal::SearchStatus s = impl_->session->searchStatus();
    SearchInfo out;
    out.active = s.active;
    out.complete = s.complete;
    out.count = s.count;
    out.current = s.current;
    if (s.currentRange) out.currentRange = termFromRowRange(*s.currentRange);
    out.pattern = s.pattern;
    return out;
}

std::optional<ElTerminal::Link> ElTerminal::linkAt(int64_t row, int col) const {
    const auto l = impl_->session->linkAt(bropty::RowPos{row, col});
    if (!l) return std::nullopt;
    return Link{termFromRowRange(l->range), l->kind, l->target, l->text};
}

std::string ElTerminal::rangeText(const Range& r) const { return impl_->session->rangeText(termToRowRange(r)); }

std::vector<ElTerminal::Command> ElTerminal::commands() const {
    std::vector<Command> out;
    auto pos = [](const bropty::RowPos& p) { return std::pair<int64_t, int>{p.row, p.col}; };
    auto opt = [&](const std::optional<bropty::RowPos>& p) {
        return p ? std::optional<std::pair<int64_t, int>>(pos(*p)) : std::nullopt;
    };
    for (const terminal::CommandInfo& c : impl_->session->commands()) {
        Command cmd;
        cmd.prompt = pos(c.prompt);
        cmd.input = opt(c.input);
        cmd.output = opt(c.output);
        cmd.end = opt(c.end);
        cmd.exitCode = c.exitCode;
        cmd.finished = c.finished;
        cmd.commandLine = c.commandLine;
        out.push_back(std::move(cmd));
    }
    return out;
}

bool ElTerminal::answerClipboard(uint64_t request, std::string_view text) {
    return impl_->session->answerClipboard(request, text);
}
bool ElTerminal::denyClipboard(uint64_t request) { return impl_->session->cancelClipboard(request); }
std::string ElTerminal::pointerShape() const { return impl_->session->pointerShape(); }

} // namespace bro::layout
