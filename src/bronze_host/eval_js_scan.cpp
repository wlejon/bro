#include "bronze_host/eval_js_scan.h"
#include "bronze_host/host_realm_scope.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace bro::bronze_host {

namespace {

enum class ScanTokKind {
    None, Ident, Number, String, TemplateEnd, RegExp,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Dot, QuestionDot, Colon, Comma, Semicolon, Arrow, PlusPlus, MinusMinus, OtherPunct,
};

struct ScanToken {
    ScanTokKind kind = ScanTokKind::None;
    std::string_view text;
    bool newlineBefore = false;
    bool parenIsControlCondition = false;  // for RParen: matching '(' followed if/while/for/with
    bool braceIsExprOrObject = false;      // for RBrace: matching '{' was in expression position
    bool opensFunctionBody = false;        // for LBrace: opens a function/method/arrow body
};

bool isScanIdentStart(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80;
}

bool isScanIdentPart(unsigned char c) {
    return isScanIdentStart(c) || (c >= '0' && c <= '9');
}

bool isExprPrefixToken(const ScanToken& tok) {
    switch (tok.kind) {
        case ScanTokKind::None: case ScanTokKind::LParen: case ScanTokKind::LBracket:
        case ScanTokKind::LBrace: case ScanTokKind::Comma: case ScanTokKind::Semicolon:
        case ScanTokKind::Colon: case ScanTokKind::Arrow: case ScanTokKind::OtherPunct:
            return true;
        case ScanTokKind::RParen: return tok.parenIsControlCondition;
        case ScanTokKind::RBrace: return !tok.braceIsExprOrObject;
        case ScanTokKind::Ident: {
            const std::string_view w = tok.text;
            return w == "return" || w == "throw" || w == "case" || w == "delete" ||
                   w == "void" || w == "typeof" || w == "yield" || w == "await" ||
                   w == "in" || w == "instanceof" || w == "of" || w == "new" ||
                   w == "do" || w == "else";
        }
        default: return false;
    }
}

bool canStartUnaryOperand(const ScanToken& next) {
    switch (next.kind) {
        case ScanTokKind::Ident: {
            const std::string_view w = next.text;
            return w != "in" && w != "instanceof" && w != "as" && w != "from" &&
                   w != "else" && w != "catch" && w != "finally" && w != "while";
        }
        case ScanTokKind::Number: case ScanTokKind::String: case ScanTokKind::TemplateEnd:
        case ScanTokKind::RegExp: case ScanTokKind::LParen: case ScanTokKind::LBrace:
        case ScanTokKind::LBracket: case ScanTokKind::PlusPlus: case ScanTokKind::MinusMinus:
            return true;
        case ScanTokKind::OtherPunct: {
            const std::string_view p = next.text;
            return p == "+" || p == "-" || p == "!" || p == "~";
        }
        default: return false;
    }
}

bool isPropertyOrBindingContext(const ScanToken& prev, const ScanToken& prevPrev) {
    if (prev.kind == ScanTokKind::Dot || prev.kind == ScanTokKind::QuestionDot) return true;
    if (prev.kind == ScanTokKind::Ident) {
        const std::string_view w = prev.text;
        if (w == "let" || w == "const" || w == "var" || w == "function" ||
            w == "class" || w == "catch" || w == "as") {
            return true;
        }
        if ((w == "get" || w == "set" || w == "static" || w == "async") && !prev.newlineBefore) {
            return true;
        }
    }
    return prev.kind == ScanTokKind::OtherPunct && prev.text == "*" &&
           prevPrev.kind == ScanTokKind::Ident && prevPrev.text == "function";
}

bool isBinaryPrefixBeforeNewline(const ScanToken& prev) {
    switch (prev.kind) {
        case ScanTokKind::LParen: case ScanTokKind::LBracket: case ScanTokKind::LBrace:
        case ScanTokKind::Comma: case ScanTokKind::Colon: case ScanTokKind::Arrow:
        case ScanTokKind::Dot: case ScanTokKind::QuestionDot: case ScanTokKind::OtherPunct:
            return true;
        case ScanTokKind::Ident: {
            const std::string_view w = prev.text;
            return w == "new" || w == "typeof" || w == "void" || w == "delete" ||
                   w == "await" || w == "yield" || w == "in" || w == "instanceof" ||
                   w == "async" || w == "function" || w == "class";
        }
        default:
            return false;
    }
}

bool isBinaryContinuationAfterNewline(const ScanToken& tok) {
    switch (tok.kind) {
        case ScanTokKind::Dot: case ScanTokKind::QuestionDot: case ScanTokKind::Comma:
        case ScanTokKind::Colon: case ScanTokKind::Arrow: case ScanTokKind::LParen:
        case ScanTokKind::LBracket:
            return true;
        case ScanTokKind::OtherPunct:
            return tok.text != "++" && tok.text != "--" && tok.text != "!" && tok.text != "~";
        case ScanTokKind::Ident:
            return tok.text == "in" || tok.text == "instanceof";
        default:
            return false;
    }
}

bool isShadowTargetName(const std::string& name) {
    return name == "name" || isBaselineHostGlobalName(name);
}

} // namespace

JsTopLevelScan scanJsTopLevel(std::string_view code) {
    const size_t n = code.size();
    size_t pos = 0;

    if (n >= 3 && static_cast<unsigned char>(code[0]) == 0xEF &&
        static_cast<unsigned char>(code[1]) == 0xBB &&
        static_cast<unsigned char>(code[2]) == 0xBF) {
        pos = 3;
    }
    if (pos + 1 < n && code[pos] == '#' && code[pos + 1] == '!') {
        pos += 2;
        while (pos < n && code[pos] != '\n' && code[pos] != '\r') ++pos;
    }
    const size_t headerEndOffset = pos;

    std::vector<ScanToken> tokens;
    tokens.reserve(128);
    std::vector<int> templateBraceDepths;
    std::vector<bool> parenControlStack;
    std::vector<bool> braceExprStack;

    auto skipQuotedString = [&](char quote) {
        ++pos;
        while (pos < n && code[pos] != quote && code[pos] != '\n' && code[pos] != '\r') {
            if (code[pos] == '\\') {
                ++pos;
                if (pos + 1 < n && code[pos] == '\r' && code[pos + 1] == '\n') ++pos;
            }
            if (pos < n) ++pos;
        }
        if (pos < n && code[pos] == quote) ++pos;
    };

    auto scanTemplateChunk = [&]() {
        ++pos;
        while (pos < n) {
            const char c = code[pos];
            if (c == '\\') { pos += (pos + 1 < n) ? 2 : 1; continue; }
            if (c == '`') { ++pos; return true; }
            if (c == '$' && pos + 1 < n && code[pos + 1] == '{') {
                pos += 2;
                templateBraceDepths.push_back(0);
                return false;
            }
            ++pos;
        }
        return true;
    };

    auto skipRegExp = [&]() {
        ++pos;
        bool inCharClass = false;
        while (pos < n && code[pos] != '\n' && code[pos] != '\r') {
            const char c = code[pos];
            if (c == '\\') { pos += (pos + 1 < n) ? 2 : 1; continue; }
            if (c == '[') inCharClass = true;
            else if (c == ']') inCharClass = false;
            else if (c == '/' && !inCharClass) { ++pos; break; }
            ++pos;
        }
        while (pos < n && isScanIdentPart(static_cast<unsigned char>(code[pos]))) ++pos;
    };

    while (pos < n) {
        bool sawNewline = false;
        while (pos < n) {
            const char c = code[pos];
            if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
                ++pos;
            } else if (c == '\n' || c == '\r') {
                sawNewline = true;
                ++pos;
            } else if (c == '/' && pos + 1 < n && code[pos + 1] == '/') {
                pos += 2;
                while (pos < n && code[pos] != '\n' && code[pos] != '\r') ++pos;
            } else if (c == '/' && pos + 1 < n && code[pos + 1] == '*') {
                pos += 2;
                while (pos < n && !(code[pos] == '*' && pos + 1 < n && code[pos + 1] == '/')) {
                    if (code[pos] == '\n' || code[pos] == '\r') sawNewline = true;
                    ++pos;
                }
                if (pos < n) pos += 2;
            } else {
                break;
            }
        }
        if (pos >= n) break;

        const ScanToken lastTok = tokens.empty() ? ScanToken{} : tokens.back();
        const char c = code[pos];
        const size_t begin = pos;

        if ((c == '}' && !templateBraceDepths.empty() && templateBraceDepths.back() == 0) || c == '`') {
            if (c == '}') {
                templateBraceDepths.pop_back();
                if (!parenControlStack.empty()) parenControlStack.pop_back();
                tokens.push_back({ScanTokKind::RParen, code.substr(begin, 1), sawNewline, false, false});
                sawNewline = false;
            }
            if (scanTemplateChunk()) {
                tokens.push_back({ScanTokKind::TemplateEnd, code.substr(begin, pos - begin), sawNewline});
            } else {
                tokens.push_back({ScanTokKind::LParen, code.substr(pos - 2, 2), sawNewline});
                parenControlStack.push_back(false);
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            skipQuotedString(c);
            tokens.push_back({ScanTokKind::String, code.substr(begin, pos - begin), sawNewline});
            continue;
        }
        if (c == '/' && isExprPrefixToken(lastTok)) {
            skipRegExp();
            tokens.push_back({ScanTokKind::RegExp, code.substr(begin, pos - begin), sawNewline});
            continue;
        }
        if (isScanIdentStart(static_cast<unsigned char>(c)) ||
            (c == '#' && pos + 1 < n && isScanIdentStart(static_cast<unsigned char>(code[pos + 1])))) {
            ++pos;
            while (pos < n && isScanIdentPart(static_cast<unsigned char>(code[pos]))) ++pos;
            tokens.push_back({ScanTokKind::Ident, code.substr(begin, pos - begin), sawNewline});
            continue;
        }
        if ((c >= '0' && c <= '9') || (c == '.' && pos + 1 < n && code[pos + 1] >= '0' && code[pos + 1] <= '9')) {
            ++pos;
            while (pos < n) {
                const char nc = code[pos];
                if ((nc >= '0' && nc <= '9') || (nc >= 'a' && nc <= 'z') ||
                    (nc >= 'A' && nc <= 'Z') || nc == '_' || nc == '.') {
                    ++pos;
                } else if ((nc == '+' || nc == '-') && pos > begin && (code[pos - 1] == 'e' || code[pos - 1] == 'E')) {
                    ++pos;
                } else {
                    break;
                }
            }
            tokens.push_back({ScanTokKind::Number, code.substr(begin, pos - begin), sawNewline});
            continue;
        }
        if (c == '(') {
            ++pos;
            const bool isCtrl = lastTok.kind == ScanTokKind::Ident &&
                                (lastTok.text == "if" || lastTok.text == "while" ||
                                 lastTok.text == "for" || lastTok.text == "with");
            parenControlStack.push_back(isCtrl);
            tokens.push_back({ScanTokKind::LParen, code.substr(begin, 1), sawNewline});
            continue;
        }
        if (c == ')') {
            ++pos;
            bool isCtrl = false;
            if (!parenControlStack.empty()) { isCtrl = parenControlStack.back(); parenControlStack.pop_back(); }
            tokens.push_back({ScanTokKind::RParen, code.substr(begin, 1), sawNewline, isCtrl, false});
            continue;
        }
        if (c == '{') {
            ++pos;
            if (!templateBraceDepths.empty()) ++templateBraceDepths.back();
            const bool isExpr = isExprPrefixToken(lastTok) && lastTok.kind != ScanTokKind::Semicolon &&
                                lastTok.kind != ScanTokKind::None && lastTok.kind != ScanTokKind::RBrace &&
                                lastTok.kind != ScanTokKind::RParen &&
                                !(lastTok.kind == ScanTokKind::Ident &&
                                  (lastTok.text == "do" || lastTok.text == "else" ||
                                   lastTok.text == "try" || lastTok.text == "finally"));
            braceExprStack.push_back(isExpr);
            tokens.push_back({ScanTokKind::LBrace, code.substr(begin, 1), sawNewline});
            continue;
        }
        if (c == '}') {
            ++pos;
            if (!templateBraceDepths.empty()) --templateBraceDepths.back();
            bool isExpr = false;
            if (!braceExprStack.empty()) { isExpr = braceExprStack.back(); braceExprStack.pop_back(); }
            tokens.push_back({ScanTokKind::RBrace, code.substr(begin, 1), sawNewline, false, isExpr});
            continue;
        }
        if (c == '[' || c == ']' || c == ';' || c == ',' || c == ':') {
            ++pos;
            const ScanTokKind k = (c == '[') ? ScanTokKind::LBracket : (c == ']') ? ScanTokKind::RBracket :
                                  (c == ';') ? ScanTokKind::Semicolon : (c == ',') ? ScanTokKind::Comma : ScanTokKind::Colon;
            tokens.push_back({k, code.substr(begin, 1), sawNewline});
            continue;
        }
        if (c == '.' && pos + 2 < n && code[pos + 1] == '.' && code[pos + 2] == '.') {
            pos += 3;
            tokens.push_back({ScanTokKind::OtherPunct, code.substr(begin, 3), sawNewline});
            continue;
        }
        if (c == '.') { ++pos; tokens.push_back({ScanTokKind::Dot, code.substr(begin, 1), sawNewline}); continue; }
        if (c == '?' && pos + 1 < n && code[pos + 1] == '.' &&
            !(pos + 2 < n && code[pos + 2] >= '0' && code[pos + 2] <= '9')) {
            pos += 2;
            tokens.push_back({ScanTokKind::QuestionDot, code.substr(begin, 2), sawNewline});
            continue;
        }
        if (c == '=' && pos + 1 < n && code[pos + 1] == '>') {
            pos += 2;
            tokens.push_back({ScanTokKind::Arrow, code.substr(begin, 2), sawNewline});
            continue;
        }
        if ((c == '+' || c == '-') && pos + 1 < n && code[pos + 1] == c) {
            pos += 2;
            tokens.push_back({c == '+' ? ScanTokKind::PlusPlus : ScanTokKind::MinusMinus, code.substr(begin, 2), sawNewline});
            continue;
        }
        ++pos;
        while (pos < n && (code[pos] == '=' || code[pos] == '&' || code[pos] == '|' ||
                           code[pos] == '?' || code[pos] == '*' || code[pos] == '<' || code[pos] == '>')) {
            ++pos;
        }
        tokens.push_back({ScanTokKind::OtherPunct, code.substr(begin, pos - begin), sawNewline});
    }

    JsTopLevelScan result;
    result.directiveEndOffset = headerEndOffset;
    const size_t tokCount = tokens.size();

    // Advance directiveEndOffset past any leading string literal directive prologue (e.g. "use strict";)
    {
        size_t dIdx = 0;
        while (dIdx < tokCount && tokens[dIdx].kind == ScanTokKind::String) {
            const ScanToken& strTok = tokens[dIdx];
            result.directiveEndOffset = static_cast<size_t>((strTok.text.data() - code.data()) + strTok.text.size());
            ++dIdx;
            if (dIdx < tokCount && tokens[dIdx].kind == ScanTokKind::Semicolon) {
                const ScanToken& semiTok = tokens[dIdx];
                result.directiveEndOffset = static_cast<size_t>((semiTok.text.data() - code.data()) + semiTok.text.size());
                ++dIdx;
            }
        }
    }

    enum class BraceScope { Normal, FunctionBody, ClassBody };
    std::vector<BraceScope> braceScopes;
    struct ConciseArrowScope { int parenDepth = 0, bracketDepth = 0, braceDepth = 0; };
    std::vector<ConciseArrowScope> conciseArrows;

    int parenDepth = 0, bracketDepth = 0, functionDepth = 0;
    bool pendingClassBody = false;
    bool inTopLevelVar = false;
    bool varExpectIdent = false;
    std::vector<std::string> currentVarNames;

    auto markBodyAfterParams = [&](size_t startParenIdx) {
        size_t k = startParenIdx;
        int pDepth = 0;
        while (k < tokCount) {
            if (tokens[k].kind == ScanTokKind::LParen) ++pDepth;
            else if (tokens[k].kind == ScanTokKind::RParen && --pDepth == 0) { ++k; break; }
            ++k;
        }
        if (k < tokCount && tokens[k].kind == ScanTokKind::Colon) {
            ++k;
            if (k < tokCount && tokens[k].kind == ScanTokKind::Ident) ++k;
        }
        if (k < tokCount && tokens[k].kind == ScanTokKind::LBrace) {
            tokens[k].opensFunctionBody = true;
        }
    };

    for (size_t i = 0; i < tokCount; ++i) {
        const ScanToken& tok = tokens[i];
        const ScanToken prev = (i > 0) ? tokens[i - 1] : ScanToken{};
        const ScanToken prevPrev = (i > 1) ? tokens[i - 2] : ScanToken{};
        const ScanToken next = (i + 1 < tokCount) ? tokens[i + 1] : ScanToken{};

        while (!conciseArrows.empty()) {
            const auto& ca = conciseArrows.back();
            const int bDepth = static_cast<int>(braceScopes.size());
            const bool same = (parenDepth == ca.parenDepth && bracketDepth == ca.bracketDepth && bDepth == ca.braceDepth);
            const bool outer = (parenDepth < ca.parenDepth || bracketDepth < ca.bracketDepth || bDepth < ca.braceDepth);
            if (outer || (same && (tok.kind == ScanTokKind::Semicolon || tok.kind == ScanTokKind::Comma ||
                                   tok.kind == ScanTokKind::RParen || tok.kind == ScanTokKind::RBracket ||
                                   tok.kind == ScanTokKind::RBrace))) {
                conciseArrows.pop_back();
                --functionDepth;
            } else {
                break;
            }
        }

        const bool atTopLevel = braceScopes.empty() && parenDepth == 0 && bracketDepth == 0 &&
                                functionDepth == 0 && conciseArrows.empty();

        if (inTopLevelVar && atTopLevel) {
            if (tok.kind == ScanTokKind::Semicolon) {
                const size_t afterSemi = static_cast<size_t>((tok.text.data() - code.data()) + tok.text.size());
                if (!currentVarNames.empty()) {
                    result.topLevelVarExports.push_back({afterSemi, std::move(currentVarNames)});
                }
                currentVarNames.clear();
                inTopLevelVar = false;
                varExpectIdent = false;
                continue;
            }
            if (tok.newlineBefore && !varExpectIdent &&
                !isBinaryPrefixBeforeNewline(prev) && !isBinaryContinuationAfterNewline(tok)) {
                const size_t beforeTok = static_cast<size_t>(tok.text.data() - code.data());
                if (!currentVarNames.empty()) {
                    result.topLevelVarExports.push_back({beforeTok, std::move(currentVarNames)});
                }
                currentVarNames.clear();
                inTopLevelVar = false;
                varExpectIdent = false;
            } else if (tok.kind == ScanTokKind::Comma) {
                varExpectIdent = true;
                continue;
            } else if (varExpectIdent) {
                if (tok.kind == ScanTokKind::Ident) {
                    if (next.kind == ScanTokKind::OtherPunct && next.text == "=") {
                        currentVarNames.emplace_back(tok.text);
                    }
                    varExpectIdent = false;
                } else if (tok.kind == ScanTokKind::LBrace || tok.kind == ScanTokKind::LBracket) {
                    varExpectIdent = false;
                }
            }
        }

        if (tok.kind == ScanTokKind::LParen) { ++parenDepth; continue; }
        if (tok.kind == ScanTokKind::RParen) { if (parenDepth > 0) --parenDepth; continue; }
        if (tok.kind == ScanTokKind::LBracket) { ++bracketDepth; continue; }
        if (tok.kind == ScanTokKind::RBracket) { if (bracketDepth > 0) --bracketDepth; continue; }
        if (tok.kind == ScanTokKind::LBrace) {
            if (tok.opensFunctionBody ||
                (!braceScopes.empty() && braceScopes.back() == BraceScope::ClassBody &&
                 prev.kind == ScanTokKind::Ident && prev.text == "static")) {
                braceScopes.push_back(BraceScope::FunctionBody);
                ++functionDepth;
            } else if (pendingClassBody) {
                pendingClassBody = false;
                braceScopes.push_back(BraceScope::ClassBody);
            } else {
                braceScopes.push_back(BraceScope::Normal);
            }
            continue;
        }
        if (tok.kind == ScanTokKind::RBrace) {
            if (!braceScopes.empty()) {
                if (braceScopes.back() == BraceScope::FunctionBody) --functionDepth;
                braceScopes.pop_back();
            }
            continue;
        }
        if (tok.kind == ScanTokKind::Arrow) {
            if (i + 1 < tokCount && tokens[i + 1].kind == ScanTokKind::LBrace) {
                tokens[i + 1].opensFunctionBody = true;
            } else {
                conciseArrows.push_back({parenDepth, bracketDepth, static_cast<int>(braceScopes.size())});
                ++functionDepth;
            }
            continue;
        }
        if (tok.kind != ScanTokKind::Ident) continue;

        const std::string_view w = tok.text;
        if (w == "function" && prev.kind != ScanTokKind::Dot && prev.kind != ScanTokKind::QuestionDot) {
            size_t k = i + 1;
            if (k < tokCount && tokens[k].kind == ScanTokKind::OtherPunct && tokens[k].text == "*") ++k;
            std::string_view fnName;
            if (k < tokCount && tokens[k].kind == ScanTokKind::Ident) {
                fnName = tokens[k].text;
                ++k;
            }
            if (k < tokCount && tokens[k].kind == ScanTokKind::LParen) markBodyAfterParams(k);
            if (atTopLevel && !inTopLevelVar && !fnName.empty()) {
                const bool asyncPrefix = (prev.kind == ScanTokKind::Ident && prev.text == "async" && !prev.newlineBefore);
                const ScanToken& beforeFn = asyncPrefix ? prevPrev : prev;
                const bool isDecl =
                    beforeFn.kind == ScanTokKind::None ||
                    beforeFn.kind == ScanTokKind::Semicolon ||
                    (beforeFn.kind == ScanTokKind::RBrace && !beforeFn.braceIsExprOrObject) ||
                    ((tok.newlineBefore || (asyncPrefix && prev.newlineBefore)) &&
                     !isBinaryPrefixBeforeNewline(beforeFn));
                if (isDecl) {
                    result.topLevelFunctionDecls.emplace_back(fnName);
                }
            }
            continue;
        }
        if (w == "class" && prev.kind != ScanTokKind::Dot && prev.kind != ScanTokKind::QuestionDot) {
            pendingClassBody = true;
            continue;
        }
        if (next.kind == ScanTokKind::LParen &&
            w != "if" && w != "for" && w != "while" && w != "switch" &&
            w != "catch" && w != "with" && w != "function" && w != "super" && w != "import") {
            const bool memberBoundary =
                (!braceScopes.empty() && braceScopes.back() == BraceScope::ClassBody) ||
                prev.kind == ScanTokKind::LBrace || prev.kind == ScanTokKind::Comma ||
                (prev.kind == ScanTokKind::Ident &&
                 (prev.text == "async" || prev.text == "get" || prev.text == "set" || prev.text == "static")) ||
                (prev.kind == ScanTokKind::OtherPunct && prev.text == "*");
            if (memberBoundary) markBodyAfterParams(i + 1);
        }

        if (atTopLevel) {
            if (w == "var" && prev.kind != ScanTokKind::Dot && prev.kind != ScanTokKind::QuestionDot &&
                !(prev.kind == ScanTokKind::Ident && prev.text == "export")) {
                inTopLevelVar = true;
                varExpectIdent = true;
                currentVarNames.clear();
                continue;
            }
            if (w == "import" && prev.kind != ScanTokKind::Dot && prev.kind != ScanTokKind::QuestionDot) {
                if (next.kind == ScanTokKind::String || next.kind == ScanTokKind::Ident ||
                    next.kind == ScanTokKind::LBrace ||
                    (next.kind == ScanTokKind::OtherPunct && next.text == "*")) {
                    result.hasModuleDecl = true;
                }
            } else if (w == "export" && prev.kind != ScanTokKind::Dot && prev.kind != ScanTokKind::QuestionDot) {
                if (next.kind == ScanTokKind::LBrace ||
                    (next.kind == ScanTokKind::OtherPunct && next.text == "*") ||
                    (next.kind == ScanTokKind::Ident &&
                     (next.text == "default" || next.text == "const" || next.text == "let" ||
                      next.text == "var" || next.text == "function" || next.text == "class" ||
                      next.text == "async"))) {
                    result.hasModuleDecl = true;
                }
            }
        }

        if (functionDepth == 0 && w == "await") {
            if (isPropertyOrBindingContext(prev, prevPrev)) continue;
            if (next.kind == ScanTokKind::Colon) continue;
            if (next.kind == ScanTokKind::LParen &&
                (prev.kind == ScanTokKind::LBrace || prev.kind == ScanTokKind::Comma ||
                 (!braceScopes.empty() && braceScopes.back() == BraceScope::ClassBody))) {
                continue;
            }
            if (prev.kind == ScanTokKind::Ident && prev.text == "for" && next.kind == ScanTokKind::LParen) {
                result.hasTopLevelAwait = true;
                continue;
            }
            if (canStartUnaryOperand(next)) {
                result.hasTopLevelAwait = true;
            }
        }
    }

    if (inTopLevelVar && !currentVarNames.empty()) {
        result.topLevelVarExports.push_back({code.size(), std::move(currentVarNames)});
    }

    return result;
}

bool hasAwaitStmt(const std::string& code) {
    return scanJsTopLevel(code).hasTopLevelAwait;
}

bool hasImportStmt(const std::string& code) {
    return scanJsTopLevel(code).hasModuleDecl;
}

std::string prepareClassicScriptGlobals(const std::string& code) {
    if (code.empty()) return code;
    JsTopLevelScan scan = scanJsTopLevel(code);
    if (scan.hasModuleDecl) return code;

    struct Insertion {
        size_t offset = 0;
        std::string text;
    };
    std::vector<Insertion> insertions;

    // 1. Hoisted top-level function declarations that shadow replaceable host globals
    {
        std::string fnPrologue;
        std::unordered_set<std::string> seenFns;
        for (const std::string& fn : scan.topLevelFunctionDecls) {
            if (fn == "name" || !isBaselineHostGlobalName(fn)) continue;
            if (!seenFns.insert(fn).second) continue;
            fnPrologue += "{globalThis." + fn + " = " + fn + ";}";
        }
        if (!fnPrologue.empty()) {
            insertions.push_back({scan.directiveEndOffset, std::move(fnPrologue)});
        }
    }

    // 2. Top-level var declarations that shadow replaceable host globals or `window.name`
    for (const TopLevelVarExport& ve : scan.topLevelVarExports) {
        std::string snippet;
        for (const std::string& vname : ve.names) {
            if (!isShadowTargetName(vname)) continue;
            if (vname == "name") {
                snippet += ";{globalThis.name = name;name = globalThis.name;}";
            } else {
                snippet += ";{globalThis." + vname + " = " + vname + ";}";
            }
        }
        if (!snippet.empty()) {
            insertions.push_back({ve.insertOffset, std::move(snippet)});
        }
    }

    if (insertions.empty()) return code;

    std::stable_sort(insertions.begin(), insertions.end(),
                     [](const Insertion& a, const Insertion& b) { return a.offset < b.offset; });

    std::string out;
    out.reserve(code.size() + insertions.size() * 48);
    size_t prev = 0;
    for (const Insertion& ins : insertions) {
        const size_t off = std::min(ins.offset, code.size());
        if (off > prev) {
            out.append(code, prev, off - prev);
            prev = off;
        }
        out.append(ins.text);
    }
    if (prev < code.size()) {
        out.append(code, prev, code.size() - prev);
    }
    return out;
}

} // namespace bro::bronze_host
