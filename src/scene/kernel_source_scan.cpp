// A brokernel source's imports and functions, without compiling it
// (kernel_source_scan.h).

#include "scene/kernel_source_scan.h"

#include <cstddef>

namespace bro::scene {

namespace {

enum class Tok { End, Ident, ColonColon, Punct };

struct Token {
    Tok kind = Tok::End;
    std::string_view text;
};

bool identStart(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool identChar(char c) { return identStart(c) || (c >= '0' && c <= '9'); }

class Lexer {
public:
    explicit Lexer(std::string_view s) : s_(s) {}

    Token next() {
        skipSpaceAndComments();
        if (i_ >= s_.size()) return {};
        const char c = s_[i_];
        if (identStart(c)) {
            const size_t start = i_;
            while (i_ < s_.size() && identChar(s_[i_])) ++i_;
            return {Tok::Ident, s_.substr(start, i_ - start)};
        }
        if (c == ':' && i_ + 1 < s_.size() && s_[i_ + 1] == ':') {
            i_ += 2;
            return {Tok::ColonColon, s_.substr(i_ - 2, 2)};
        }
        if (c == '"') {
            ++i_;
            while (i_ < s_.size() && s_[i_] != '"' && s_[i_] != '\n') {
                if (s_[i_] == '\\') ++i_;
                ++i_;
            }
            if (i_ < s_.size()) ++i_;
            return {Tok::Punct, "\""};
        }
        ++i_;
        return {Tok::Punct, s_.substr(i_ - 1, 1)};
    }

private:
    void skipSpaceAndComments() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
            } else if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '/') {
                while (i_ < s_.size() && s_[i_] != '\n') ++i_;
            } else if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '*') {
                const size_t end = s_.find("*/", i_ + 2);
                i_ = end == std::string_view::npos ? s_.size() : end + 2;
            } else {
                return;
            }
        }
    }

    std::string_view s_;
    size_t i_ = 0;
};

std::vector<Token> tokens(std::string_view source) {
    std::vector<Token> out;
    Lexer lex(source);
    for (Token t = lex.next(); t.kind != Tok::End; t = lex.next()) out.push_back(t);
    return out;
}

bool isPunct(const std::vector<Token>& t, size_t i, char c) {
    return i < t.size() && t[i].kind == Tok::Punct && t[i].text.size() == 1 && t[i].text[0] == c;
}
bool isWord(const std::vector<Token>& t, size_t i, std::string_view w) {
    return i < t.size() && t[i].kind == Tok::Ident && t[i].text == w;
}

// A path `a::b::c` from token `i`: its segments; `i` left after it.
std::vector<std::string_view> path(const std::vector<Token>& t, size_t& i) {
    std::vector<std::string_view> segs;
    if (i >= t.size() || t[i].kind != Tok::Ident) return segs;
    segs.push_back(t[i++].text);
    while (i + 1 < t.size() && t[i].kind == Tok::ColonColon && t[i + 1].kind == Tok::Ident) {
        segs.push_back(t[i + 1].text);
        i += 2;
    }
    return segs;
}

std::string join(const std::vector<std::string_view>& segs, size_t n) {
    std::string s;
    for (size_t k = 0; k < n; ++k) {
        if (k) s += "::";
        s += segs[k];
    }
    return s;
}

// After a path: `as name` and then `;`.
bool endsItem(const std::vector<Token>& t, size_t i) {
    if (isWord(t, i, "as")) i += 2;
    return isPunct(t, i, ';');
}

}  // namespace

std::vector<std::string> kernelSourceImports(std::string_view source) {
    const std::vector<Token> t = tokens(source);
    std::vector<std::string> out;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i].kind != Tok::Ident) continue;
        if (i > 0 && t[i - 1].kind == Tok::ColonColon) continue;  // a path's segment, not a keyword
        if (t[i].text == "import") {
            size_t j = i + 1;
            const auto segs = path(t, j);
            if (!segs.empty() && endsItem(t, j)) out.push_back(join(segs, segs.size()));
        } else if (t[i].text == "from") {
            size_t j = i + 1;
            const auto segs = path(t, j);
            if (!segs.empty() && isWord(t, j, "import")) out.push_back(join(segs, segs.size()));
        } else if (t[i].text == "use") {
            size_t j = i + 1;
            const auto segs = path(t, j);
            if (segs.empty()) continue;
            if (j < t.size() && t[j].kind == Tok::ColonColon && (isPunct(t, j + 1, '{') || isPunct(t, j + 1, '*'))) {
                if (isPunct(t, j + 1, '{') || endsItem(t, j + 2)) out.push_back(join(segs, segs.size()));
            } else if (segs.size() >= 2 && endsItem(t, j)) {
                out.push_back(join(segs, segs.size() - 1));
            }
        }
    }
    return out;
}

bool kernelSourceDefinesFn(std::string_view source, std::string_view name) {
    const std::vector<Token> t = tokens(source);
    for (size_t i = 0; i + 2 < t.size(); ++i) {
        if (isWord(t, i, "fn") && isWord(t, i + 1, name) && isPunct(t, i + 2, '(')) return true;
    }
    return false;
}

}  // namespace bro::scene
