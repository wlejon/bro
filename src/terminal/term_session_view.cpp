// TermSession's view half: the scrollback position, selection gestures,
// search, links and mouse reports. Everything here takes the session lock
// (the parser holds it for one slice at most) and wakes the parser thread,
// which publishes the change with the next frame.

#include "terminal/term_session.h"

#include <bropty/links.h>
#include <bropty/search_regex.h>

#include <algorithm>

namespace bro::terminal {

void TermSession::afterViewChange() { wake(); }

ViewState TermSession::viewState() const {
    std::lock_guard<std::mutex> g(mu_);
    const bropty::Terminal& t = session_.terminal();
    ViewState v;
    v.topRow = view_->top_row();
    v.firstRow = t.first_row();
    v.screenTopRow = t.screen_top_row();
    v.rows = t.rows();
    v.atBottom = view_->at_bottom();
    v.altScreen = t.alt_screen_active();
    return v;
}

void TermSession::scrollBy(int64_t rows) {
    if (rows == 0) return;
    {
        std::lock_guard<std::mutex> g(mu_);
        view_->scroll_by(rows);
    }
    afterViewChange();
}

void TermSession::scrollToRow(int64_t row) {
    {
        std::lock_guard<std::mutex> g(mu_);
        view_->scroll_to_row(row);
    }
    afterViewChange();
}

void TermSession::scrollToBottom() {
    {
        std::lock_guard<std::mutex> g(mu_);
        view_->scroll_to_bottom();
    }
    afterViewChange();
}

void TermSession::scrollToTop() {
    {
        std::lock_guard<std::mutex> g(mu_);
        view_->scroll_to_row(session_.terminal().first_row());
    }
    afterViewChange();
}

bool TermSession::scrollToPrompt(bool backward) {
    bool moved;
    {
        std::lock_guard<std::mutex> g(mu_);
        moved = view_->scroll_to_prompt(backward);
    }
    if (moved) afterViewChange();
    return moved;
}

// ---- selection -----------------------------------------------------------------

void TermSession::selectStart(bropty::RowPos cell, bropty::SelectionMode mode, bool rightHalf) {
    {
        std::lock_guard<std::mutex> g(mu_);
        view_->selection().start(cell, mode, rightHalf);
    }
    afterViewChange();
}

void TermSession::selectExtend(bropty::RowPos cell, bool rightHalf) {
    {
        std::lock_guard<std::mutex> g(mu_);
        bropty::Selection& sel = view_->selection();
        if (!sel.active()) return;
        sel.extend(cell, rightHalf);
    }
    afterViewChange();
}

void TermSession::selectClear() {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!view_->selection().active()) return;
        view_->selection().clear();
    }
    afterViewChange();
}

void TermSession::selectAll() {
    {
        std::lock_guard<std::mutex> g(mu_);
        view_->selection().select_all();
    }
    afterViewChange();
}

bool TermSession::selectOutput(std::optional<bropty::RowPos> cell) {
    bool ok;
    {
        std::lock_guard<std::mutex> g(mu_);
        bropty::Selection& sel = view_->selection();
        ok = cell ? sel.select_output(*cell) : sel.select_last_output();
        if (ok) view_->reveal(sel.range());
    }
    afterViewChange();
    return ok;
}

bool TermSession::selectionActive() const {
    std::lock_guard<std::mutex> g(mu_);
    return view_->selection().active();
}

std::optional<bropty::RowRange> TermSession::selectionRange() const {
    std::lock_guard<std::mutex> g(mu_);
    const bropty::Selection& sel = view_->selection();
    if (!sel.active()) return std::nullopt;
    return sel.range();
}

bool TermSession::selectionIsBlock() const {
    std::lock_guard<std::mutex> g(mu_);
    return view_->selection().active() && view_->selection().is_block();
}

// ---- search ----------------------------------------------------------------------

bool TermSession::searchStart(std::string_view pattern, const SearchOptions& opts, std::string* error) {
    if (pattern.empty()) {
        searchClear();
        return true;
    }
    bropty::RegexSearchOptions ro;
    ro.literal = !opts.regex;
    ro.whole_word = opts.wholeWord;
    ro.case_mode = opts.caseMode == SearchOptions::Case::Sensitive ? bropty::SearchCase::Sensitive
                 : opts.caseMode == SearchOptions::Case::Insensitive ? bropty::SearchCase::Insensitive
                 : bropty::SearchCase::Smart;
    std::shared_ptr<bropty::RegexMatcher> matcher = bropty::RegexMatcher::create(pattern, ro, error);
    if (!matcher) return false;
    {
        std::lock_guard<std::mutex> g(mu_);
        view_->search().start(std::move(matcher));
        searchPattern_.assign(pattern);
    }
    afterViewChange();  // the parser steps it through history from here
    return true;
}

void TermSession::searchClear() {
    {
        std::lock_guard<std::mutex> g(mu_);
        view_->search().clear();
        searchPattern_.clear();
    }
    afterViewChange();
}

std::optional<bropty::RowRange> TermSession::searchNext(bool backward) {
    std::optional<bropty::RowRange> r;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!view_->search().active()) return std::nullopt;
        r = view_->search_next(backward);
    }
    afterViewChange();
    return r;
}

SearchStatus TermSession::searchStatus() const {
    std::lock_guard<std::mutex> g(mu_);
    // Output parsed since the last published frame counts too (a frame is
    // built only once the renderer took the previous one).
    view_->sync();
    const bropty::Search& s = view_->search();
    SearchStatus st;
    st.active = s.active();
    if (!st.active) return st;
    st.complete = s.complete();
    st.count = s.size();
    st.current = s.current_index();
    st.currentRange = s.current();
    st.pattern = searchPattern_;
    return st;
}

// ---- links -------------------------------------------------------------------------

namespace {

const char* linkKindName(bropty::LinkKind k) {
    switch (k) {
        case bropty::LinkKind::Hyperlink: return "hyperlink";
        case bropty::LinkKind::Url: return "url";
        case bropty::LinkKind::Path: return "path";
    }
    return "url";
}

} // namespace

std::optional<LinkInfo> TermSession::linkAt(bropty::RowPos cell) const {
    std::lock_guard<std::mutex> g(mu_);
    const bropty::Terminal& t = session_.terminal();
    if (cell.row < t.first_row() || cell.row >= t.end_row() || cell.col < 0 || cell.col >= t.cols())
        return std::nullopt;
    std::optional<bropty::LinkHit> hit = bropty::link_at(t, cell);
    if (!hit) return std::nullopt;
    LinkInfo li;
    li.range = hit->range;
    li.kind = linkKindName(hit->kind);
    li.target = hit->target;
    bropty::Selection probe(t);
    probe.select_range(hit->range);
    bropty::TextOptions to;
    to.newline = "";
    li.text = probe.text(to);
    return li;
}

void TermSession::setHover(std::optional<bropty::RowPos> cell) {
    {
        std::lock_guard<std::mutex> g(mu_);
        const auto& cur = view_->hover();
        // Moving within the same link (or over no link) changes nothing seen.
        if (cell) {
            const auto hit = bropty::link_at(session_.terminal(), *cell);
            if (!hit && !cur) return;
            if (hit && cur && hit->range == cur->range && hit->target == cur->target) return;
        } else if (!cur) {
            return;
        }
        view_->set_hover(cell);
    }
    afterViewChange();
}

std::string TermSession::rowsText(int64_t first, int64_t end) const {
    std::lock_guard<std::mutex> g(mu_);
    const bropty::Terminal& t = session_.terminal();
    first = std::max(first, t.first_row());
    end = std::min(end, t.end_row());
    std::string out;
    for (int64_t r = first; r < end; ++r) {
        if (r > first) out.push_back('\n');
        out += t.row_at(r).text();
    }
    return out;
}

std::string TermSession::rangeText(bropty::RowRange range) const {
    std::lock_guard<std::mutex> g(mu_);
    if (range.empty()) return {};
    bropty::Selection probe(session_.terminal());
    probe.select_range(range);
    return probe.text();
}

// ---- mouse ---------------------------------------------------------------------------

bool TermSession::sendMouse(const bropty::MouseEvent& ev) {
    std::lock_guard<std::mutex> g(mu_);
    if (!pty_ || exited()) return false;
    return session_.send_mouse(ev);
}

bropty::MouseTracking TermSession::mouseTracking() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_.terminal().modes().mouse_tracking;
}

bool TermSession::altScreen() const {
    std::lock_guard<std::mutex> g(mu_);
    return session_.terminal().alt_screen_active();
}

} // namespace bro::terminal
