// A brokernel source's imports and functions, without compiling it
// (kernel_source_scan.h).

#include "scene/kernel_source_scan.h"

#include <cstddef>
#include <mutex>
#include <unordered_map>

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

namespace {

bool isAssignOp(const std::vector<Token>& t, size_t idx, size_t end) {
    if (idx >= end || t[idx].kind != Tok::Punct || t[idx].text.size() != 1) return false;
    const char c = t[idx].text[0];
    if (c == '=') return !isPunct(t, idx + 1, '=');
    if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '&' || c == '|' || c == '^') {
        return idx + 1 < end && isPunct(t, idx + 1, '=');
    }
    if ((c == '<' || c == '>') && idx + 2 < end && isPunct(t, idx + 1, c) && isPunct(t, idx + 2, '=')) {
        return true;
    }
    return false;
}

}  // namespace

bool kernelSourceModifiesVertexPos(std::string_view source) {
    const std::vector<Token> t = tokens(source);
    int braceDepth = 0;
    for (size_t i = 0; i + 2 < t.size(); ++i) {
        if (isPunct(t, i, '{')) {
            ++braceDepth;
            continue;
        }
        if (isPunct(t, i, '}')) {
            if (braceDepth > 0) --braceDepth;
            continue;
        }
        if (braceDepth != 0) continue;
        if (!isWord(t, i, "fn") || !isWord(t, i + 1, "vertex") || !isPunct(t, i + 2, '(')) continue;

        // Locate parameter list `(...)`.
        size_t p = i + 3;
        int parenDepth = 1;
        size_t closeParen = t.size();
        for (size_t q = p; q < t.size(); ++q) {
            if (isPunct(t, q, '(')) ++parenDepth;
            else if (isPunct(t, q, ')') && --parenDepth == 0) {
                closeParen = q;
                break;
            }
        }
        if (closeParen >= t.size()) return true;

        // First parameter name (skipping optional `inout` / `in` / `out`).
        if (p < closeParen && (isWord(t, p, "inout") || isWord(t, p, "in") || isWord(t, p, "out"))) ++p;
        if (p >= closeParen || t[p].kind != Tok::Ident) return true;
        const std::string_view vParam = t[p].text;

        // Locate `{ ... }` body and check for `->` return type.
        bool hasReturnType = false;
        size_t bodyBrace = t.size();
        for (size_t q = closeParen + 1; q < t.size(); ++q) {
            if (isPunct(t, q, '{')) {
                bodyBrace = q;
                break;
            }
            if (isPunct(t, q, '-') && q + 1 < t.size() && isPunct(t, q + 1, '>')) hasReturnType = true;
        }
        if (bodyBrace >= t.size()) return true;

        int d = 1;
        size_t bodyEnd = t.size();
        for (size_t q = bodyBrace + 1; q < t.size(); ++q) {
            if (isPunct(t, q, '{')) ++d;
            else if (isPunct(t, q, '}') && --d == 0) {
                bodyEnd = q;
                break;
            }
        }
        if (bodyEnd >= t.size()) return true;

        const size_t bodyStart = bodyBrace + 1;
        std::vector<std::string_view> vertexVars{vParam};
        auto isVertexVar = [&](std::string_view w) {
            for (const auto& v : vertexVars) {
                if (v == w) return true;
            }
            return false;
        };

        for (size_t k = bodyStart; k < bodyEnd; ++k) {
            if (isWord(t, k, "set_vertex_position")) return true;

            // `var lhs [: Type] = rhs;` / `let lhs [: Type] = rhs;` copy tracking.
            if ((isWord(t, k, "var") || isWord(t, k, "let")) && k + 1 < bodyEnd && t[k + 1].kind == Tok::Ident) {
                const std::string_view lhs = t[k + 1].text;
                size_t eqIdx = bodyEnd;
                size_t semiIdx = bodyEnd;
                for (size_t q = k + 2; q < bodyEnd; ++q) {
                    if (isPunct(t, q, ';')) {
                        semiIdx = q;
                        break;
                    }
                    if (eqIdx == bodyEnd && isPunct(t, q, '=') && !isPunct(t, q + 1, '=') &&
                        !isPunct(t, q - 1, '<') && !isPunct(t, q - 1, '>') && !isPunct(t, q - 1, '!')) {
                        eqIdx = q;
                    }
                }
                const bool copiedVertexVar =
                    eqIdx < semiIdx && eqIdx + 2 == semiIdx && t[eqIdx + 1].kind == Tok::Ident &&
                    isVertexVar(t[eqIdx + 1].text);
                if (copiedVertexVar) {
                    if (!isVertexVar(lhs)) vertexVars.push_back(lhs);
                } else if (isVertexVar(lhs)) {
                    return true;
                }
            }

            // Any write to `.pos` (e.g. `.pos =`, `.pos.x +=`, `.pos[0] =`).
            if (isPunct(t, k, '.') && k + 1 < bodyEnd && isWord(t, k + 1, "pos")) {
                size_t m = k + 2;
                while (m < bodyEnd) {
                    if (isPunct(t, m, '.') && m + 1 < bodyEnd && t[m + 1].kind == Tok::Ident) {
                        m += 2;
                    } else if (isPunct(t, m, '[')) {
                        int bd = 1;
                        ++m;
                        while (m < bodyEnd && bd > 0) {
                            if (isPunct(t, m, '[')) ++bd;
                            else if (isPunct(t, m, ']')) --bd;
                            ++m;
                        }
                    } else {
                        break;
                    }
                }
                if (isAssignOp(t, m, bodyEnd)) return true;
            }

            // Struct literal field `pos: ...`.
            if (isWord(t, k, "pos") && k + 1 < bodyEnd && isPunct(t, k + 1, ':') &&
                (k == bodyStart || !isPunct(t, k - 1, '.'))) {
                return true;
            }

            // Reassigning a Vertex variable or passing it to a helper function.
            if (t[k].kind == Tok::Ident && isVertexVar(t[k].text)) {
                const bool isDeclName = k > bodyStart && (isWord(t, k - 1, "var") || isWord(t, k - 1, "let"));
                const bool isFieldName = k > bodyStart && isPunct(t, k - 1, '.');
                if (!isDeclName && !isFieldName) {
                    if (k > bodyStart && isPunct(t, k - 1, '&')) return true;
                    if (isAssignOp(t, k + 1, bodyEnd)) return true;
                    if (k + 1 < bodyEnd && (isPunct(t, k + 1, ',') || isPunct(t, k + 1, ')'))) return true;
                }
            }

            // `return` must be empty (for void/inout) or `return <vertexVar>;`.
            if (isWord(t, k, "return")) {
                if (k + 1 < bodyEnd && isPunct(t, k + 1, ';')) {
                    if (hasReturnType) return true;
                } else if (k + 2 < bodyEnd && t[k + 1].kind == Tok::Ident && isVertexVar(t[k + 1].text) &&
                           isPunct(t, k + 2, ';')) {
                    // Passthrough return of an unmodified-pos Vertex copy.
                } else {
                    return true;
                }
            }
        }
        return false;
    }
    return false;
}

bool kernelFnReadsField(std::string_view source, std::string_view fn, std::string_view field) {
    const std::vector<Token> t = tokens(source);
    // Every function's body: [first token, closing brace).
    struct Body {
        std::string_view name;
        size_t begin, end;
    };
    std::vector<Body> bodies;
    for (size_t i = 0; i + 2 < t.size(); ++i) {
        if (!isWord(t, i, "fn") || t[i + 1].kind != Tok::Ident || !isPunct(t, i + 2, '(')) continue;
        size_t q = i + 3;
        while (q < t.size() && !isPunct(t, q, '{') && !isPunct(t, q, ';')) ++q;
        if (q >= t.size() || !isPunct(t, q, '{')) continue;
        int d = 1;
        size_t e = q + 1;
        for (; e < t.size() && d > 0; ++e) {
            if (isPunct(t, e, '{')) ++d;
            else if (isPunct(t, e, '}')) --d;
        }
        bodies.push_back({t[i + 1].text, q + 1, e});
        i = e - 1;
    }
    auto find = [&](std::string_view name) -> const Body* {
        for (const Body& b : bodies) {
            if (b.name == name) return &b;
        }
        return nullptr;
    };
    std::vector<std::string_view> todo{fn}, seen;
    while (!todo.empty()) {
        const std::string_view name = todo.back();
        todo.pop_back();
        bool visited = false;
        for (const auto& s : seen) visited = visited || s == name;
        if (visited) continue;
        seen.push_back(name);
        const Body* b = find(name);
        if (!b) continue;
        for (size_t k = b->begin; k < b->end; ++k) {
            if (isPunct(t, k, '.') && isWord(t, k + 1, field)) return true;
            // A call of one of the source's own functions (not a path's
            // segment, as `mesh::f(`, which is another module's).
            if (t[k].kind == Tok::Ident && isPunct(t, k + 1, '(') && (k == 0 || t[k - 1].kind != Tok::ColonColon) &&
                find(t[k].text)) {
                todo.push_back(t[k].text);
            }
        }
    }
    return false;
}

KernelVertexScan scanKernelVertex(const std::string& source) {
    // A control path (a node given a shader), so a lock; a scene of a
    // thousand nodes on one hook scans it once.
    static std::mutex mu;
    static std::unordered_map<std::string, KernelVertexScan> seen;
    {
        std::lock_guard<std::mutex> lock(mu);
        if (auto it = seen.find(source); it != seen.end()) return it->second;
    }
    KernelVertexScan scan;
    scan.hasVertex = kernelSourceDefinesFn(source, "vertex");
    scan.hasVertexPos = scan.hasVertex && kernelSourceModifiesVertexPos(source);
    std::lock_guard<std::mutex> lock(mu);
    if (seen.size() >= 512) seen.clear();      // edits under hot reload are new sources
    seen.emplace(source, scan);
    return scan;
}

}  // namespace bro::scene
