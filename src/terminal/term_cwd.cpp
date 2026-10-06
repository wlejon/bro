#include "terminal/term_cwd.h"

#include <algorithm>
#include <cctype>

namespace bro::terminal {

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// %XX sequences decoded; a malformed one stays as written.
std::string percentDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hexValue(s[i + 1]);
            const int lo = hexValue(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(char(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// "/C:/x", "/C|/x", "/c:" -> "C:\x", "C:\x", "C:\"; anything else unchanged.
std::string windowsDrive(std::string path) {
    if (path.size() < 3 || path[0] != '/' || !isAlpha(path[1]) || (path[2] != ':' && path[2] != '|')) return path;
    if (path.size() > 3 && path[3] != '/' && path[3] != '\\') return path;
    std::string out;
    out.push_back(char(std::toupper(static_cast<unsigned char>(path[1]))));
    out.push_back(':');
    out.append(path, 3, std::string::npos);
    if (out.size() == 2) out.push_back('/');
    std::replace(out.begin(), out.end(), '/', '\\');
    return out;
}

bool startsWithNoCase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != prefix[i]) return false;
    return true;
}

} // namespace

CwdLocation cwdFromUri(std::string_view uri) {
    CwdLocation out;
    const size_t sep = uri.find("://");
    // A scheme is letters, digits, '+', '-', '.', starting with a letter.
    bool scheme = sep != std::string_view::npos && sep > 0 && isAlpha(uri[0]);
    for (size_t i = 0; scheme && i < sep; ++i) {
        const char c = uri[i];
        scheme = isAlpha(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
    }
    if (!scheme) {
        out.path = std::string(uri);
        return out;
    }
    const bool file = startsWithNoCase(uri, "file://");
    std::string_view rest = uri.substr(sep + 3);
    const size_t slash = rest.find('/');
    out.host = std::string(rest.substr(0, slash));
    if (slash == std::string_view::npos) return out;
    std::string_view path = rest.substr(slash);
    if (file) {
        path = path.substr(0, path.find_first_of("?#"));
        out.path = windowsDrive(percentDecode(path));
    } else {
        out.path = windowsDrive(std::string(path));
    }
    return out;
}

} // namespace bro::terminal
