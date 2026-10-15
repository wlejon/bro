#include "engine/agent_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>

namespace bro::engine::view {

namespace {

constexpr std::string_view kFlagChars = "*!^@";

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool allDigits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!isDigit(c)) return false;
    return true;
}

long rounded(float v) { return std::lround(v); }

std::string rectText(const Item& it) {
    char b[96];
    std::snprintf(b, sizeof(b), "%ld,%ld %ldx%ld", rounded(it.x), rounded(it.y), rounded(it.w), rounded(it.h));
    return b;
}

bool sameRect(const Item& a, const Item& b) {
    if (a.hasRect != b.hasRect) return false;
    return !a.hasRect || (rounded(a.x) == rounded(b.x) && rounded(a.y) == rounded(b.y) &&
                          rounded(a.w) == rounded(b.w) && rounded(a.h) == rounded(b.h));
}

// A quoted string starting at s[i] ('"'); i ends past the closing quote.
bool readQuoted(std::string_view s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return true;
        if (c == '\\' && i < s.size()) c = s[i++];
        out += c;
    }
    return false;
}

std::string_view nextToken(std::string_view s, size_t& i) {
    while (i < s.size() && s[i] == ' ') ++i;
    size_t j = i;
    while (j < s.size() && s[j] != ' ') ++j;
    std::string_view t = s.substr(i, j - i);
    i = j;
    return t;
}

bool parseInt(std::string_view s, long& out) {
    bool neg = false;
    if (!s.empty() && s[0] == '-') {
        neg = true;
        s.remove_prefix(1);
    }
    if (!allDigits(s)) return false;
    long v = 0;
    for (char c : s) v = v * 10 + (c - '0');
    out = neg ? -v : v;
    return true;
}

std::string stripIndent(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    return line.substr(i);
}

bool sameSnapshot(const Snapshot& a, const Snapshot& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].handle != b[i].handle || a[i].depth != b[i].depth || !sameItem(a[i], b[i], false)) return false;
    return true;
}

}  // namespace

std::string truncateUtf8(std::string_view s, size_t maxChars) {
    size_t chars = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) continue;
        if (chars == maxChars) return std::string(s.substr(0, i)) + "\xE2\x80\xA6";  // …
        ++chars;
    }
    return std::string(s);
}

std::string quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

bool isWindowHandle(std::string_view s) { return s.size() > 1 && s[0] == 'w' && allDigits(s.substr(1)); }

bool isElementHandle(std::string_view s) {
    const size_t dot = s.find('.');
    return dot != std::string_view::npos && allDigits(s.substr(0, dot)) && allDigits(s.substr(dot + 1));
}

int windowOf(std::string_view h) {
    long v = -1;
    if (isWindowHandle(h)) parseInt(h.substr(1), v);
    else if (isElementHandle(h)) parseInt(h.substr(0, h.find('.')), v);
    return static_cast<int>(v);
}

uint32_t elementOf(std::string_view h) {
    if (!isElementHandle(h)) return 0;
    long v = 0;
    parseInt(h.substr(h.find('.') + 1), v);
    return static_cast<uint32_t>(v);
}

std::string formatItem(const Item& it, bool rects) {
    std::string out;
    if (!it.isWindow()) out.assign(static_cast<size_t>(1 + std::max(0, it.depth)), ' ');
    // A window's flags follow its handle ("w3* app"), an element's its role
    // ("3.3 option*").
    out += it.handle;
    if (it.isWindow()) out += it.flags;
    out += ' ';
    out += it.role.empty() ? "-" : it.role;
    if (!it.isWindow()) out += it.flags;
    if (!it.name.empty()) {
        out += ' ';
        out += quote(truncateUtf8(it.name, kMaxNameChars));
    }
    if (it.hasValue) {
        out += " =";
        out += quote(truncateUtf8(it.value, kMaxNameChars));
    }
    if (!it.extra.empty()) {
        out += " [";
        out += it.extra;
        out += ']';
    }
    if (it.hasRect && (rects || it.isWindow())) {
        out += ' ';
        out += rectText(it);
    }
    return out;
}

bool parseItem(std::string_view line, Item& out) {
    out = Item{};
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    const size_t indent = i;
    auto splitFlags = [](std::string_view tok, std::string& head, std::string& flags) {
        size_t f = tok.size();
        while (f > 0 && kFlagChars.find(tok[f - 1]) != std::string_view::npos) --f;
        head = std::string(tok.substr(0, f));
        flags = std::string(tok.substr(f));
    };
    std::string_view handleTok = nextToken(line, i);
    std::string handle, windowFlags;
    splitFlags(handleTok, handle, windowFlags);
    const bool window = isWindowHandle(handle);
    if (!window && (!isElementHandle(handle) || !windowFlags.empty())) return false;
    out.handle = handle;
    out.depth = window ? 0 : std::max(0, static_cast<int>(indent) - 1);
    std::string_view role = nextToken(line, i);
    if (role.empty()) return false;
    if (window) {
        out.role = std::string(role);
        out.flags = windowFlags;
    } else {
        splitFlags(role, out.role, out.flags);
    }
    if (out.role == "-") out.role.clear();
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        if (i >= line.size()) break;
        if (line[i] == '"') {
            if (!readQuoted(line, i, out.name)) return false;
        } else if (line[i] == '=' && i + 1 < line.size() && line[i + 1] == '"') {
            ++i;
            if (!readQuoted(line, i, out.value)) return false;
            out.hasValue = true;
        } else if (line[i] == '[') {
            const size_t close = line.find(']', i);
            if (close == std::string_view::npos) return false;
            out.extra = std::string(line.substr(i + 1, close - i - 1));
            i = close + 1;
        } else {
            std::string_view pos = nextToken(line, i);
            std::string_view size = nextToken(line, i);
            const size_t comma = pos.find(','), x = size.find('x');
            long px, py, sw, sh;
            if (comma == std::string_view::npos || x == std::string_view::npos ||
                !parseInt(pos.substr(0, comma), px) || !parseInt(pos.substr(comma + 1), py) ||
                !parseInt(size.substr(0, x), sw) || !parseInt(size.substr(x + 1), sh))
                return false;
            out.hasRect = true;
            out.x = static_cast<float>(px);
            out.y = static_cast<float>(py);
            out.w = static_cast<float>(sw);
            out.h = static_cast<float>(sh);
        }
    }
    return true;
}

namespace {

// Whether `b` continues a run after `a`: sibling lines of one role and the
// same flags, nothing but a name each, numbered one after another.
bool continuesRun(const Item& a, const Item& b) {
    return !a.isWindow() && !b.isWindow() && windowOf(a.handle) == windowOf(b.handle) && a.depth == b.depth &&
           a.role == b.role && a.flags == b.flags && !a.hasValue && !b.hasValue && a.extra.empty() &&
           b.extra.empty() && elementOf(b.handle) == elementOf(a.handle) + 1;
}

// "3.21-23 button "7" "8" "9"": a run's line, after `indent`.
std::string runLine(const std::vector<const Item*>& run) {
    const Item& first = *run.front();
    std::string out = first.handle + "-" + std::to_string(elementOf(run.back()->handle)) + " " +
                      (first.role.empty() ? std::string("-") : first.role) + first.flags;
    for (const Item* it : run) out += " " + quote(truncateUtf8(it->name, kMaxNameChars));
    return out;
}

// A run "N.A-B" line back into its items (B - A + 1 of them, one name each).
bool parseRunLine(std::string_view line, Snapshot& out) {
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    const int depth = std::max(0, static_cast<int>(i) - 1);
    std::string_view handleTok = nextToken(line, i);
    const size_t dash = handleTok.find('-');
    if (dash == std::string_view::npos) return false;
    const std::string_view first = handleTok.substr(0, dash);
    long last = 0;
    if (!isElementHandle(first) || !parseInt(handleTok.substr(dash + 1), last)) return false;
    const long from = elementOf(first);
    if (last < from) return false;
    Item proto;
    proto.depth = depth;
    std::string_view role = nextToken(line, i);
    size_t f = role.size();
    while (f > 0 && kFlagChars.find(role[f - 1]) != std::string_view::npos) --f;
    proto.role = std::string(role.substr(0, f));
    proto.flags = std::string(role.substr(f));
    if (proto.role == "-") proto.role.clear();
    const std::string prefix = std::string(first.substr(0, first.find('.') + 1));
    for (long n = from; n <= last; ++n) {
        while (i < line.size() && line[i] == ' ') ++i;
        Item it = proto;
        it.handle = prefix + std::to_string(n);
        if (!readQuoted(line, i, it.name)) return false;
        out.push_back(std::move(it));
    }
    return true;
}

constexpr size_t kMinRun = 3;

// The lines of `items` (each after its own indent), runs of kMinRun or more
// joined into one line (not with rects: each line carries its own then).
std::string joinLines(const std::vector<const Item*>& items, const std::vector<std::string>& indents, bool rects,
                      const char* lead) {
    std::string out;
    for (size_t i = 0; i < items.size();) {
        size_t j = i + 1;
        if (!rects)
            while (j < items.size() && indents[j] == indents[i] && continuesRun(*items[j - 1], *items[j])) ++j;
        if (j - i >= kMinRun) {
            out += lead + indents[i] + runLine(std::vector<const Item*>(items.begin() + i, items.begin() + j)) + "\n";
            i = j;
            continue;
        }
        out += lead + indents[i] + stripIndent(formatItem(*items[i], rects)) + "\n";
        ++i;
    }
    return out;
}

}  // namespace

Snapshot parseItems(std::string_view text) {
    Snapshot out;
    size_t i = 0;
    while (i <= text.size()) {
        size_t j = text.find('\n', i);
        if (j == std::string_view::npos) j = text.size();
        Item it;
        const std::string_view line = text.substr(i, j - i);
        if (parseItem(line, it)) out.push_back(std::move(it));
        else parseRunLine(line, out);
        i = j + 1;
    }
    return out;
}

std::string formatBody(const Snapshot& snap, bool rects) {
    std::string out;
    int section = -1;
    std::vector<const Item*> items;
    std::vector<std::string> indents;
    auto flush = [&]() {
        out += joinLines(items, indents, rects, "");
        items.clear();
        indents.clear();
    };
    for (const Item& it : snap) {
        if (!it.isWindow()) {
            const int w = windowOf(it.handle);
            if (w != section) {
                flush();
                out += 'w' + std::to_string(w) + ":\n";
                section = w;
            }
        }
        items.push_back(&it);
        indents.push_back(it.isWindow() ? std::string() : std::string(static_cast<size_t>(1 + it.depth), ' '));
    }
    flush();
    return out;
}

bool sameItem(const Item& a, const Item& b, bool rects) {
    if (a.role != b.role || a.flags != b.flags || a.name != b.name || a.hasValue != b.hasValue ||
        a.value != b.value || a.extra != b.extra)
        return false;
    // A window's rect is part of its line; an element's only when asked.
    if ((rects || a.isWindow()) && !sameRect(a, b)) return false;
    return true;
}

std::string formatDiff(const Snapshot& from, const Snapshot& to, bool rects, DiffCounts* counts) {
    DiffCounts c;
    std::unordered_map<std::string_view, const Item*> before, after;
    for (const Item& it : from) before.emplace(it.handle, &it);
    for (const Item& it : to) after.emplace(it.handle, &it);
    std::string added, changed, removed;
    // A run of added lines keeps its nesting, relative to the run's
    // shallowest line so far: a dialog's fields sit under the dialog.
    bool inRun = false;
    int runBase = 0;
    std::vector<const Item*> run;
    std::vector<std::string> runIndents;
    auto flushRun = [&]() {
        added += joinLines(run, runIndents, rects, "+ ");
        run.clear();
        runIndents.clear();
    };
    for (const Item& it : to) {
        auto b = before.find(it.handle);
        if (b == before.end()) {
            const int depth = it.isWindow() ? 0 : it.depth;
            if (!inRun || depth < runBase) runBase = depth;
            inRun = true;
            run.push_back(&it);
            runIndents.push_back(std::string(static_cast<size_t>(depth - runBase), ' '));
            ++c.added;
            continue;
        }
        if (inRun) flushRun();
        inRun = false;
        const Item& old = *b->second;
        if (sameItem(old, it, rects)) continue;
        std::string line = "~ " + it.handle;
        if (old.role != it.role || old.flags != it.flags) {
            // The flags it has now, then "-x" for each it lost: "~ 6.6
            // textbox -@" is a field that lost the keyboard. A window's role
            // (its app) does not change, so its line is "~ w3*" / "~ w0 -*".
            std::string lost;
            for (char f : old.flags)
                if (it.flags.find(f) == std::string::npos) lost += f;
            if (it.isWindow()) {
                line += it.flags;
                if (old.role != it.role) line += " " + (it.role.empty() ? std::string("-") : it.role);
            } else {
                line += " " + (it.role.empty() ? std::string("-") : it.role) + it.flags;
            }
            if (!lost.empty()) line += " -" + lost;
        }
        if (old.name != it.name) line += " " + quote(truncateUtf8(it.name, kMaxNameChars));
        if (old.hasValue != it.hasValue || old.value != it.value)
            line += " =" + quote(it.hasValue ? truncateUtf8(it.value, kMaxNameChars) : std::string());
        if (old.extra != it.extra) line += " [" + (it.extra.empty() ? std::string("none") : it.extra) + "]";
        if ((rects || it.isWindow()) && !sameRect(old, it) && it.hasRect) line += " " + rectText(it);
        changed += line + "\n";
        ++c.changed;
    }
    flushRun();
    // Removed: by handle, except a window none of whose elements are left,
    // which is "N.*" (the focus moved away from it, or it closed).
    std::map<int, std::pair<int, bool>> perWindow;  // window -> (elements before, any left)
    for (const Item& it : from)
        if (!it.isWindow()) {
            auto& pw = perWindow[windowOf(it.handle)];
            ++pw.first;
            if (after.count(it.handle)) pw.second = true;
        }
    // Handles numbered one after another in a window are one range, "2.56-67".
    std::set<int> collapsed;
    int rangeWin = -1;
    uint32_t rangeFrom = 0, rangeTo = 0;
    auto flushRange = [&]() {
        if (rangeWin < 0) return;
        removed += (removed.empty() ? "- " : " ") + std::to_string(rangeWin) + "." + std::to_string(rangeFrom);
        if (rangeTo > rangeFrom) removed += "-" + std::to_string(rangeTo);
        rangeWin = -1;
    };
    for (const Item& it : from) {
        if (after.count(it.handle)) continue;
        ++c.removed;
        if (!it.isWindow()) {
            const int w = windowOf(it.handle);
            const auto& pw = perWindow[w];
            if (pw.first > 1 && !pw.second) {
                if (!collapsed.insert(w).second) continue;
                flushRange();
                removed += (removed.empty() ? "- " : " ") + std::to_string(w) + ".*";
                continue;
            }
            const uint32_t n = elementOf(it.handle);
            if (rangeWin == w && n == rangeTo + 1) {
                rangeTo = n;
                continue;
            }
            flushRange();
            rangeWin = w;
            rangeFrom = rangeTo = n;
            continue;
        }
        flushRange();
        removed += (removed.empty() ? "- " : " ") + it.handle;
    }
    flushRange();
    if (!removed.empty()) removed += "\n";
    if (counts) *counts = c;
    return added + removed + changed;
}

std::string header(uint64_t rev) { return "@view r" + std::to_string(rev); }
std::string noChange(uint64_t rev) { return header(rev) + " =\n"; }

uint64_t History::commit(const std::string& scope, Snapshot snap) {
    auto& q = scopes_[scope];
    if (!q.empty() && sameSnapshot(q.back().snap, snap)) {
        q.back().snap = std::move(snap);
        return q.back().rev;
    }
    q.push_back(Entry{next_++, std::move(snap)});
    while (q.size() > keep_) q.pop_front();
    return q.back().rev;
}

const Snapshot* History::find(const std::string& scope, uint64_t rev) const {
    auto it = scopes_.find(scope);
    if (it == scopes_.end()) return nullptr;
    for (const Entry& e : it->second)
        if (e.rev == rev) return &e.snap;
    return nullptr;
}

const Snapshot* History::latest(const std::string& scope, uint64_t* rev) const {
    auto it = scopes_.find(scope);
    if (it == scopes_.end() || it->second.empty()) return nullptr;
    if (rev) *rev = it->second.back().rev;
    return &it->second.back().snap;
}

std::string History::reply(const std::string& scope, uint64_t since, bool rects) const {
    uint64_t rev = 0;
    const Snapshot* cur = latest(scope, &rev);
    if (!cur) return header(0) + "\n";
    if (since == 0) return header(rev) + "\n" + formatBody(*cur, rects);
    if (since == rev) return noChange(rev);
    const Snapshot* old = find(scope, since);
    if (!old) return header(rev) + " full\n" + formatBody(*cur, rects);
    DiffCounts c;
    std::string body = formatDiff(*old, *cur, rects, &c);
    if (c.empty()) return noChange(rev);
    return header(rev) + " +" + std::to_string(c.added) + " -" + std::to_string(c.removed) + " ~" +
           std::to_string(c.changed) + "\n" + body;
}

}  // namespace bro::engine::view
