// What the program says, as DOM events on the <terminal> element, and the
// view's changes the page tracks (scroll position, search status).
//
//   titlechange    {title}                       OSC 0 / 2
//   cwdchange      {cwd}                         OSC 7 (a file:// URI)
//   bell           {}                            BEL
//   notification   {title, body, id, source, urgency}  OSC 9 / 777 / 99
//   progress       {state, value}                OSC 9;4
//   clipboardwrite {text, selection}             OSC 52 write; cancelable
//   clipboardread  {id, selection}               OSC 52 read; cancelable
//   promptmark     {mark, params, exitCode}      OSC 133 A / B / C / D
//   foregroundchange {process}                   {pid, name, path, commandLine} or null
//   scroll         {topRow, firstRow, screenTopRow, rows, atBottom, altScreen}
//   searchchange   {active, complete, count, current}
//
// A clipboardwrite not cancelled is written to the clipboard (the primary
// selection too when the program names it); a clipboardread not cancelled is
// answered from the clipboard, and a cancelled one waits for the page's
// answerClipboard(id, text) / denyClipboard(id). The element's clipboard
// policy decides whether either reaches the page at all (term_session_host.cpp).

#include "layout/el_terminal_impl.h"

#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"

#include <cstdio>
#include <cstdlib>

namespace bro::layout {

std::string termJsonString(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(ch);
                }
        }
    }
    out.push_back('"');
    return out;
}

bool termDispatch(dom::Element* el, const char* type, const std::string& detailJson, bool cancelable) {
    if (!el) return true;
    dom::CustomEvent evt(type, false, cancelable);
    evt.setDetail(detailJson);
    evt.setIsTrusted(true);
    dom::dispatchDomEvent(el, evt);
    return !evt.defaultPrevented();
}

namespace {

const char* progressStateName(int state) {
    switch (state) {
        case 1: return "normal";
        case 2: return "error";
        case 3: return "indeterminate";
        case 4: return "paused";
        default: return "none";
    }
}

// OSC 52's selection field names the clipboard ('c'), the primary ('p') or
// the X selection ('s'); none means "s 0", which every terminal treats as
// the clipboard.
bool namesPrimary(const std::string& sel) {
    return sel.find('p') != std::string::npos && sel.find('c') == std::string::npos;
}

std::string viewJson(const terminal::ViewState& v) {
    return "{\"topRow\":" + std::to_string(v.topRow) + ",\"firstRow\":" + std::to_string(v.firstRow) +
           ",\"screenTopRow\":" + std::to_string(v.screenTopRow) + ",\"rows\":" + std::to_string(v.rows) +
           ",\"historyRows\":" + std::to_string(v.historyRows()) +
           ",\"atBottom\":" + (v.atBottom ? "true" : "false") + ",\"altScreen\":" + (v.altScreen ? "true" : "false") +
           "}";
}

} // namespace

bool ElTerminal::takeCursorChanged() {
    const bool c = impl_->cursorChanged;
    impl_->cursorChanged = false;
    return c;
}

void ElTerminal::dispatchEvents() {
    Impl& m = *impl_;
    for (terminal::TermEvent& e : m.session->takeEvents()) {
        if (!elem_) return;
        using K = terminal::TermEvent::Kind;
        switch (e.kind) {
            case K::Title:
                termDispatch(elem_, "titlechange", "{\"title\":" + termJsonString(e.text) + "}");
                break;
            case K::Cwd:
                termDispatch(elem_, "cwdchange", "{\"cwd\":" + termJsonString(e.text) + "}");
                break;
            case K::Bell:
                termDispatch(elem_, "bell", "{}");
                break;
            case K::Notification:
                termDispatch(elem_, "notification",
                             "{\"title\":" + termJsonString(e.title) + ",\"body\":" + termJsonString(e.text) +
                                 ",\"id\":" + termJsonString(e.id) + ",\"source\":" + termJsonString(e.source) +
                                 ",\"urgency\":" + std::to_string(e.number) + "}");
                break;
            case K::Progress:
                termDispatch(elem_, "progress",
                             std::string("{\"state\":\"") + progressStateName(e.number) +
                                 "\",\"value\":" + std::to_string(e.value) + "}");
                break;
            case K::ClipboardWrite: {
                const bool ok = termDispatch(elem_, "clipboardwrite",
                                             "{\"text\":" + termJsonString(e.text) +
                                                 ",\"selection\":" + termJsonString(e.selection) + "}",
                                             /*cancelable=*/true);
                const Host& h = host();
                if (ok && h.writeClipboard) h.writeClipboard(e.text, namesPrimary(e.selection));
                break;
            }
            case K::ClipboardRead: {
                const bool ok = termDispatch(elem_, "clipboardread",
                                             "{\"id\":" + std::to_string(e.request) +
                                                 ",\"selection\":" + termJsonString(e.selection) + "}",
                                             /*cancelable=*/true);
                if (ok) {
                    // A listener may have answered it already; then this is a no-op.
                    const Host& h = host();
                    if (h.readClipboard) m.session->answerClipboard(e.request, h.readClipboard(namesPrimary(e.selection)));
                    else m.session->cancelClipboard(e.request);
                }
                break;
            }
            case K::PromptMark: {
                std::string exit = "null";
                if (e.mark == 'D') {
                    // "D;<exit code>[;...]": the parameters after the mark letter.
                    const std::string& p = e.params;
                    size_t i = p.find_first_of("-0123456789");
                    if (i != std::string::npos) {
                        char* end = nullptr;
                        const long v = std::strtol(p.c_str() + i, &end, 10);
                        if (end != p.c_str() + i) exit = std::to_string(v);
                    }
                }
                termDispatch(elem_, "promptmark",
                             "{\"mark\":\"" + std::string(1, e.mark) + "\",\"params\":" + termJsonString(e.params) +
                                 ",\"exitCode\":" + exit + "}");
                break;
            }
            case K::PointerShape:
                m.cursorChanged = true;
                break;
            case K::Foreground: {
                // The answer now, not the one queued: changes coalesce.
                std::string json = "null";
                if (const auto p = foregroundProcess())
                    json = "{\"pid\":" + std::to_string(p->pid) + ",\"name\":" + termJsonString(p->name) +
                           ",\"path\":" + termJsonString(p->path) +
                           ",\"commandLine\":" + termJsonString(p->commandLine) + "}";
                termDispatch(elem_, "foregroundchange", "{\"process\":" + json + "}");
                break;
            }
        }
    }

    // The view, as the frame presents it.
    if (!m.frame || !elem_) return;
    const bropty::Frame& f = *m.frame;
    terminal::ViewState v;
    v.topRow = f.top_row;
    v.firstRow = f.first_row;
    v.screenTopRow = f.screen_top_row;
    v.rows = f.rows;
    v.atBottom = f.at_bottom();
    v.altScreen = f.alt_screen;
    if (!m.haveView || !(v == m.lastView)) {
        m.haveView = true;
        m.lastView = v;
        termDispatch(elem_, "scroll", viewJson(v));
    }
    terminal::SearchStatus s;
    s.active = f.search_active;
    s.complete = f.search_complete;
    s.count = f.match_count;
    s.current = f.current_match;
    if (!(s == m.lastSearch)) {
        m.lastSearch = s;
        termDispatch(elem_, "searchchange",
                     std::string("{\"active\":") + (s.active ? "true" : "false") +
                         ",\"complete\":" + (s.complete ? "true" : "false") + ",\"count\":" + std::to_string(s.count) +
                         ",\"current\":" + (s.current ? std::to_string(*s.current) : std::string("null")) + "}");
    }
}

} // namespace bro::layout
