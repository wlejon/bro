#include "engine/agent_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

Snapshot parseItems(std::string_view text) {
    Snapshot out;
    size_t i = 0;
    while (i <= text.size()) {
        size_t j = text.find('\n', i);
        if (j == std::string_view::npos) j = text.size();
        Item it;
        if (parseItem(text.substr(i, j - i), it)) out.push_back(std::move(it));
        i = j + 1;
    }
    return out;
}

std::string formatBody(const Snapshot& snap, bool rects) {
    std::string out;
    int section = -1;
    for (const Item& it : snap) {
        if (!it.isWindow()) {
            const int w = windowOf(it.handle);
            if (w != section) {
                out += 'w' + std::to_string(w) + ":\n";
                section = w;
            }
        }
        out += formatItem(it, rects);
        out += '\n';
    }
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
    for (const Item& it : to) {
        auto b = before.find(it.handle);
        if (b == before.end()) {
            added += "+ " + stripIndent(formatItem(it, rects)) + "\n";
            ++c.added;
            continue;
        }
        const Item& old = *b->second;
        if (sameItem(old, it, rects)) continue;
        std::string line = "~ " + it.handle;
        if (old.role != it.role || old.flags != it.flags) {
            const std::string role = it.role.empty() ? "-" : it.role;
            line += it.isWindow() ? it.flags + " " + role : " " + role + it.flags;
        }
        if (old.name != it.name) line += " " + quote(truncateUtf8(it.name, kMaxNameChars));
        if (old.hasValue != it.hasValue || old.value != it.value)
            line += " =" + quote(it.hasValue ? truncateUtf8(it.value, kMaxNameChars) : std::string());
        if (old.extra != it.extra) line += " [" + it.extra + "]";
        if ((rects || it.isWindow()) && !sameRect(old, it) && it.hasRect) line += " " + rectText(it);
        changed += line + "\n";
        ++c.changed;
    }
    for (const Item& it : from) {
        if (after.count(it.handle)) continue;
        removed += (removed.empty() ? "- " : " ") + it.handle;
        ++c.removed;
    }
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
