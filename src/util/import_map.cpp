#include "util/import_map.h"

#include "util/log.h"
#include "util/remote_asset.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace bro::util {
namespace {

// ---------------------------------------------------------------------------
// The smallest JSON reader that can read an import map.
//
// An import map is an object of objects of strings, so that is exactly what
// this parses. Keys we do not implement are skipped *structurally* rather than
// rejected: a map carrying `scopes` or `integrity` is still a map whose
// `imports` we must honour, and refusing the whole document over a key we were
// never going to read would fail the page for no reason.
// ---------------------------------------------------------------------------
struct Scanner {
    const std::string& s;
    size_t i = 0;

    explicit Scanner(const std::string& src) : s(src) {}

    void ws() {
        while (i < s.size() &&
               (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
            ++i;
        }
    }
    bool eat(char c) {
        ws();
        if (i < s.size() && s[i] == c) { ++i; return true; }
        return false;
    }
    bool peek(char c) {
        ws();
        return i < s.size() && s[i] == c;
    }

    bool string(std::string& out) {
        ws();
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return false;
            switch (s[i++]) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    // A \u escape is legal JSON, but an import-map key or target
                    // is a specifier that has to become a path. Accept the ASCII
                    // range, where the code unit *is* the byte, and refuse the
                    // rest rather than emit a mangled filename that would fail
                    // later with a misleading "no such file".
                    if (i + 4 > s.size()) return false;
                    unsigned v = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s[i + k];
                        v <<= 4;
                        if (h >= '0' && h <= '9')      v |= unsigned(h - '0');
                        else if (h >= 'a' && h <= 'f') v |= unsigned(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v |= unsigned(h - 'A' + 10);
                        else return false;
                    }
                    i += 4;
                    if (v == 0 || v > 0x7f) return false;
                    out += char(v);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    /// Skip one complete JSON value of any shape.
    bool skipValue() {
        ws();
        if (i >= s.size()) return false;
        const char c = s[i];
        if (c == '"') { std::string t; return string(t); }
        if (c == '{' || c == '[') {
            const char open = c;
            const char close = (c == '{') ? '}' : ']';
            ++i;
            int depth = 1;
            while (i < s.size() && depth > 0) {
                const char d = s[i];
                // A brace inside a string is not structure — consume the whole
                // string so it cannot unbalance the count.
                if (d == '"') { std::string t; if (!string(t)) return false; continue; }
                if (d == open) ++depth;
                else if (d == close) --depth;
                ++i;
            }
            return depth == 0;
        }
        // number / true / false / null — runs to the next structural character.
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
               s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') {
            ++i;
        }
        return true;
    }
};

bool isRooted(const std::string& p) {
    if (p.empty()) return false;
    if (p.front() == '/' || p.front() == '\\') return true;
    // Drive-letter absolute (Windows).
    return p.size() > 2 && p[1] == ':' && (p[2] == '/' || p[2] == '\\');
}

/// Resolve one import-map target against the map's base directory, and
/// lexically canonicalize it so that the loader's module cache — which is
/// keyed on this string — sees one spelling per file.
std::string resolveTarget(const std::string& target, const std::string& baseDir) {
    if (target.empty()) return {};

    // A target may be an absolute URL — `"three-mesh-bvh": "https://cdn/…"` is
    // how a page pins a dependency it does not vendor. It resolves to itself,
    // and must not go through the path normalization below, which would eat the
    // "//" after the scheme.
    if (hasUrlScheme(target)) return target;

    std::string joined;
    if (isRooted(target)) {
        joined = target;
    } else {
        joined = baseDir;
        if (!joined.empty() && joined.back() != '/' && joined.back() != '\\') {
            joined += '/';
        }
        joined += target;
    }

    // A target ending in '/' is a *directory* prefix: the rest of the specifier
    // gets appended to it. Normalization is allowed to drop that trailing
    // separator, which would silently concatenate "…/jsm" with "loaders/x.js".
    const bool wantsTrailingSeparator =
        target.back() == '/' || target.back() == '\\';

    std::string normalized =
        std::filesystem::path(joined).lexically_normal().string();
    if (normalized.empty()) normalized = joined;

    if (wantsTrailingSeparator && !normalized.empty() &&
        normalized.back() != '/' && normalized.back() != '\\') {
        normalized += static_cast<char>(std::filesystem::path::preferred_separator);
    }
    return normalized;
}

} // namespace

bool ImportMap::parse(const std::string& json, const std::string& baseDir) {
    Scanner sc(json);
    if (!sc.eat('{')) return false;

    std::vector<std::pair<std::string, std::string>> exact;
    std::vector<std::pair<std::string, std::string>> prefix;
    bool sawScopes = false;

    if (!sc.peek('}')) {
        do {
            std::string key;
            if (!sc.string(key)) return false;
            if (!sc.eat(':')) return false;

            if (key != "imports") {
                if (key == "scopes") sawScopes = true;
                if (!sc.skipValue()) return false;
                continue;
            }

            if (!sc.eat('{')) return false;
            if (!sc.peek('}')) {
                do {
                    std::string spec, target;
                    if (!sc.string(spec)) return false;
                    if (!sc.eat(':')) return false;
                    // A null target means "block this specifier" on the web. We
                    // have no way to express a block, so anything that is not a
                    // string is skipped rather than guessed at.
                    if (!sc.peek('"')) { if (!sc.skipValue()) return false; continue; }
                    if (!sc.string(target)) return false;
                    if (spec.empty()) continue;

                    const std::string resolved = resolveTarget(target, baseDir);
                    if (resolved.empty()) continue;

                    if (spec.back() == '/') prefix.emplace_back(spec, resolved);
                    else                    exact.emplace_back(spec, resolved);
                } while (sc.eat(','));
            }
            if (!sc.eat('}')) return false;
        } while (sc.eat(','));
    }
    if (!sc.eat('}')) return false;

    if (sawScopes) {
        LOG_WARN("[importmap] this map declares \"scopes\", which is not implemented — "
                 "every specifier resolves through the top-level \"imports\" instead. A "
                 "module a scope meant to redirect will load the global one.");
    }

    // Longest key first: the spec's most-specific-prefix-wins rule then falls
    // out of the first hit of a linear scan in resolve().
    std::sort(prefix.begin(), prefix.end(),
              [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });

    exact_ = std::move(exact);
    prefix_ = std::move(prefix);
    return true;
}

std::string ImportMap::resolve(const std::string& specifier) const {
    for (const auto& [key, target] : exact_) {
        if (specifier == key) return target;
    }
    for (const auto& [key, target] : prefix_) {
        if (specifier.size() > key.size() &&
            specifier.compare(0, key.size(), key) == 0) {
            // The target already carries its trailing separator; appending the
            // remainder can still mix '/' with '\' on Windows, so normalize the
            // join for the same module-cache reason resolveTarget does — unless
            // the target is a URL, whose separators are already the right ones.
            std::string joined = target + specifier.substr(key.size());
            if (hasUrlScheme(target)) return joined;
            std::string normalized =
                std::filesystem::path(joined).lexically_normal().string();
            return normalized.empty() ? joined : normalized;
        }
    }
    return {};
}

} // namespace bro::util
