// HTMLTerminalElement's view: the scrollback position, selection, search,
// links, the shell's commands (OSC 133) and the answers to a program's
// clipboard reads. Rows are absolute (they keep counting as history grows,
// so a row names the same line until it leaves history); a range is
// { startRow, startCol, endRow, endCol }, end exclusive between cell
// boundaries. docs/terminal-api.js is the contract.

#include "bronze_host/host_element_terminal.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_values.h"

#include "layout/el_terminal.h"

#include <cmath>
#include <string>

namespace bro::bronze_host {

namespace {

using Term = layout::ElTerminal;

Term* control(Value self) { return hostTerminalControl(self, true); }

Value rangeValue(const Term::Range& r) {
    ObjectBuilder o;
    o.set("startRow", ev::fromDouble(double(r.startRow)));
    o.set("startCol", ev::fromDouble(r.startCol));
    o.set("endRow", ev::fromDouble(double(r.endRow)));
    o.set("endCol", ev::fromDouble(r.endCol));
    return o.get();
}

Value rangeOrNull(const std::optional<Term::Range>& r) { return r ? rangeValue(*r) : ev::null(); }

Value posValue(const std::pair<int64_t, int>& p) {
    ObjectBuilder o;
    o.set("row", ev::fromDouble(double(p.first)));
    o.set("col", ev::fromDouble(p.second));
    return o.get();
}

Value posOrNull(const std::optional<std::pair<int64_t, int>>& p) { return p ? posValue(*p) : ev::null(); }

double finiteNum(Value v) {
    if (ev::isObject(v) || ev::isUndefined(v) || ev::isNull(v)) return 0.0;
    const double d = ev::toDouble(v);
    return std::isfinite(d) ? d : 0.0;
}

bool readRange(Value v, Term::Range& r) {
    if (!ev::isObject(v)) return false;
    ev::Persistent obj(v);
    r.startRow = int64_t(finiteNum(ev::getProperty(obj.get(), "startRow")));
    r.startCol = int(finiteNum(ev::getProperty(obj.get(), "startCol")));
    r.endRow = int64_t(finiteNum(ev::getProperty(obj.get(), "endRow")));
    r.endCol = int(finiteNum(ev::getProperty(obj.get(), "endCol")));
    return true;
}

Value scrollResult(Value self, Term::ScrollOp op, int64_t amount) {
    Term* t = control(self);
    return ev::fromBool(t && t->scroll(op, amount));
}

}  // namespace

void decorateTerminalViewProto(ObjectBuilder& b) {
    // ---- the scrollback position ------------------------------------------
    b.accessor("viewport",
        [](Value self, std::span<const Value>) -> Value {
            Term* t = control(self);
            if (!t) return ev::undefined();
            const Term::ViewInfo v = t->viewInfo();
            ObjectBuilder o;
            o.set("topRow", ev::fromDouble(double(v.topRow)));
            o.set("firstRow", ev::fromDouble(double(v.firstRow)));
            o.set("screenTopRow", ev::fromDouble(double(v.screenTopRow)));
            o.set("rows", ev::fromDouble(v.rows));
            o.set("historyRows", ev::fromDouble(double(v.screenTopRow - v.firstRow)));
            o.set("atBottom", ev::fromBool(v.atBottom));
            o.set("altScreen", ev::fromBool(v.altScreen));
            return o.get();
        },
        nullptr);
    b.def("scrollLines", 1, [](Value self, std::span<const Value> a) -> Value {
        return scrollResult(self, Term::ScrollOp::Lines, int64_t(finiteNum(argAt(a, 0))));
    });
    b.def("scrollPages", 1, [](Value self, std::span<const Value> a) -> Value {
        return scrollResult(self, Term::ScrollOp::Pages, int64_t(finiteNum(argAt(a, 0))));
    });
    b.def("scrollToTop", 0, [](Value self, std::span<const Value>) -> Value {
        return scrollResult(self, Term::ScrollOp::Top, 0);
    });
    b.def("scrollToBottom", 0, [](Value self, std::span<const Value>) -> Value {
        return scrollResult(self, Term::ScrollOp::Bottom, 0);
    });
    b.def("scrollToRow", 1, [](Value self, std::span<const Value> a) -> Value {
        return scrollResult(self, Term::ScrollOp::ToRow, int64_t(finiteNum(argAt(a, 0))));
    });
    // scrollToPrompt(direction): -1 the previous prompt, +1 the next one.
    b.def("scrollToPrompt", 1, [](Value self, std::span<const Value> a) -> Value {
        const bool back = finiteNum(argAt(a, 0)) < 0;
        return scrollResult(self, back ? Term::ScrollOp::PreviousPrompt : Term::ScrollOp::NextPrompt, 0);
    });

    // ---- selection ------------------------------------------------------------
    b.def("select", 1, [](Value self, std::span<const Value> a) -> Value {
        Term* t = control(self);
        Term::Range r;
        if (!readRange(argAt(a, 0), r)) return ev::throwTypeError("select(range): range must be an object");
        if (t) t->selectRange(r);
        return ev::undefined();
    });
    b.def("selectAll", 0, [](Value self, std::span<const Value>) -> Value {
        if (Term* t = control(self)) t->selectAll();
        return ev::undefined();
    });
    b.def("clearSelection", 0, [](Value self, std::span<const Value>) -> Value {
        if (Term* t = control(self)) t->clearSelection();
        return ev::undefined();
    });
    // selectOutput([row, col]): the output of the command at that cell, or of
    // the last command. False when there is none.
    b.def("selectOutput", 2, [](Value self, std::span<const Value> a) -> Value {
        Term* t = control(self);
        if (!t) return ev::fromBool(false);
        std::optional<std::pair<int64_t, int>> cell;
        if (hasArg(a, 0)) cell = std::pair<int64_t, int>{int64_t(finiteNum(a[0])), int(finiteNum(argAt(a, 1)))};
        return ev::fromBool(t->selectOutput(cell));
    });
    b.accessor("selection",
        [](Value self, std::span<const Value>) -> Value {
            Term* t = control(self);
            return t ? rangeOrNull(t->selectionRange()) : ev::null();
        },
        nullptr);
    b.def("selectionText", 0, [](Value self, std::span<const Value>) -> Value {
        Term* t = control(self);
        return ev::fromUtf8(t ? t->selectionText() : std::string());
    });
    b.def("textInRange", 1, [](Value self, std::span<const Value> a) -> Value {
        Term* t = control(self);
        Term::Range r;
        if (!readRange(argAt(a, 0), r)) return ev::throwTypeError("textInRange(range): range must be an object");
        return ev::fromUtf8(t ? t->rangeText(r) : std::string());
    });

    // ---- search ---------------------------------------------------------------
    // search(pattern, { regex, caseMode, wholeWord }): matches the whole
    // buffer (history first, in the background; `searchchange` reports
    // progress). An empty pattern clears. Throws SyntaxError on a bad regex.
    b.def("search", 2, [](Value self, std::span<const Value> a) -> Value {
        Term* t = control(self);
        if (!t) return ev::fromBool(false);
        Term::SearchQuery q;
        q.pattern = hasArg(a, 0) && !ev::isNull(a[0]) ? ev::toUtf8(a[0]) : std::string();
        if (hasArg(a, 1) && ev::isObject(a[1])) {
            ev::Persistent o(a[1]);
            Value v = ev::getProperty(o.get(), "regex");
            q.regex = !ev::isUndefined(v) && ev::toBool(v);
            v = ev::getProperty(o.get(), "wholeWord");
            q.wholeWord = !ev::isUndefined(v) && ev::toBool(v);
            v = ev::getProperty(o.get(), "caseMode");
            if (!ev::isUndefined(v) && !ev::isNull(v)) q.caseMode = ev::toUtf8(v);
        }
        std::string error;
        if (!t->search(q, &error)) {
            if (q.caseMode != "smart" && q.caseMode != "sensitive" && q.caseMode != "insensitive")
                return ev::throwTypeError(error.c_str());
            return ev::throwValue(hostMakeDomError("SyntaxError", "search(): " + error));
        }
        return ev::fromBool(true);
    });
    b.def("clearSearch", 0, [](Value self, std::span<const Value>) -> Value {
        if (Term* t = control(self)) t->clearSearch();
        return ev::undefined();
    });
    // searchNext() / searchPrevious(): the next match (wrapping), scrolled
    // into view and made current; null when there is none.
    b.def("searchNext", 0, [](Value self, std::span<const Value>) -> Value {
        Term* t = control(self);
        return t ? rangeOrNull(t->searchNext(false)) : ev::null();
    });
    b.def("searchPrevious", 0, [](Value self, std::span<const Value>) -> Value {
        Term* t = control(self);
        return t ? rangeOrNull(t->searchNext(true)) : ev::null();
    });
    b.accessor("searchStatus",
        [](Value self, std::span<const Value>) -> Value {
            Term* t = control(self);
            if (!t) return ev::undefined();
            const Term::SearchInfo s = t->searchInfo();
            ObjectBuilder o;
            o.set("active", ev::fromBool(s.active));
            o.set("complete", ev::fromBool(s.complete));
            o.set("count", ev::fromDouble(double(s.count)));
            o.set("current", s.current ? ev::fromDouble(double(*s.current)) : ev::null());
            ev::Persistent range(rangeOrNull(s.currentRange));
            o.set("range", range.get());
            o.set("pattern", ev::fromUtf8(s.pattern));
            return o.get();
        },
        nullptr);

    // ---- links ------------------------------------------------------------------
    b.def("linkAt", 2, [](Value self, std::span<const Value> a) -> Value {
        Term* t = control(self);
        if (!t) return ev::null();
        const auto l = t->linkAt(int64_t(finiteNum(argAt(a, 0))), int(finiteNum(argAt(a, 1))));
        if (!l) return ev::null();
        ObjectBuilder o;
        ev::Persistent range(rangeValue(l->range));
        o.set("range", range.get());
        o.set("kind", ev::fromUtf8(l->kind));
        o.set("uri", ev::fromUtf8(l->target));
        o.set("text", ev::fromUtf8(l->text));
        return o.get();
    });

    // ---- the shell's commands (OSC 133) ---------------------------------------
    b.accessor("commands",
        [](Value self, std::span<const Value>) -> Value {
            Term* t = control(self);
            if (!t) return makeEmptyArray();
            const std::vector<Term::Command> list = t->commands();
            return hostArrayOf(list.size(), [&list](size_t i) -> Value {
                const Term::Command& c = list[i];
                ObjectBuilder o;
                ev::Persistent p(posValue(c.prompt));
                o.set("prompt", p.get());
                p.set(posOrNull(c.input));
                o.set("input", p.get());
                p.set(posOrNull(c.output));
                o.set("output", p.get());
                p.set(posOrNull(c.end));
                o.set("end", p.get());
                o.set("exitCode", c.exitCode ? ev::fromDouble(*c.exitCode) : ev::null());
                o.set("finished", ev::fromBool(c.finished));
                o.set("commandLine", ev::fromUtf8(c.commandLine));
                return o.get();
            });
        },
        nullptr);

    // ---- a program's clipboard read, answered by the page ---------------------
    b.def("answerClipboard", 2, [](Value self, std::span<const Value> a) -> Value {
        Term* t = control(self);
        const std::string text = hasArg(a, 1) && !ev::isNull(a[1]) ? ev::toUtf8(a[1]) : std::string();
        return ev::fromBool(t && t->answerClipboard(uint64_t(finiteNum(argAt(a, 0))), text));
    });
    b.def("denyClipboard", 1, [](Value self, std::span<const Value> a) -> Value {
        Term* t = control(self);
        return ev::fromBool(t && t->denyClipboard(uint64_t(finiteNum(argAt(a, 0)))));
    });
}

}  // namespace bro::bronze_host
