#include "bronze_host/eval_jit.h"
#include "bronze_host/app_aot.h"
#include "bronze_host/eval.h"
#include "bronze_host/host_gc.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_headless.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_brokit.h"
#include "bronze_host/host_pins.h"
#include "bronze_host/host_rejection_events.h"
#include "engine/engine.h"
#include "engine/app_runtime.h"
#include "util/asset_mounts.h"
#include "util/log.h"
#include "util/user_dirs.h"

#include "api/api.h"  // brokit::api::RequireDirGuard

#include "eval/eval.h"
#include "embed/embed.h"
#include "modules/modules.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <future>
#include <thread>
#include <vector>

namespace bro::bronze_host {

bool isJitDisabled() {
    static bool disabled = [] {
        const char* val = std::getenv("BRO_DISABLE_JIT");
        if (!val) val = std::getenv("BRO_EVAL_AOT");
        if (!val) val = std::getenv("BRO_NO_JIT");
        return val && (std::strcmp(val, "1") == 0 || std::strcmp(val, "true") == 0);
    }();
    return disabled;
}

void applyJitTierOverride() {
    const char* val = std::getenv("BRO_JIT_TIER");
    if (!val || !*val) return;
    if (auto tier = bronze::parseExecutionTier(val)) {
        bronze::eval::setDefaultTier(*tier);
        LOG_INFO("JS runs at the '%s' tier (BRO_JIT_TIER)", std::string(bronze::executionTierToString(*tier)).c_str());
    } else {
        LOG_WARN("BRO_JIT_TIER='%s' names no tier (0, 1, 2 or auto); JS runs tiered", val);
    }
}

void stopBackgroundCompiles() {
    bronze::eval::stopBackgroundCompiles();
}

namespace {

// A throw out of a script's top level, reported with the script it came out
// of. The value's own text is `thrownValueText`'s: a `stack` when one was set,
// else `Name: message`. bronze records no source position on an Error — no
// line, no stack (runtime/exception.h) — so the script name is the location
// the report can give; `throw new Error(...)` sites in a test are what carry
// the rest.
bool reportCallResult(engine::Engine& engine, const bronze::embed::CallResult& res,
                      const char* context, const std::string& filename) {
    if (res.thrown) {
        // The page's `error` handlers see a script's uncaught throw, as they
        // do on the web. The run still counts as failed and is still logged:
        // a script that died at its top level did not finish, whatever a
        // handler made of it.
        bronze::embed::Persistent thrownRoot(res.value);
        hostDispatchUncaughtError(thrownRoot.get());
        std::string errMsg = thrownValueText(thrownRoot.get());
        LOG_ERROR("%s: uncaught %s (in %s)", context, errMsg.c_str(),
                  filename.empty() ? "<eval>" : filename.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }
    return true;
}

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

struct JsTopLevelScan {
    bool hasTopLevelAwait = false;
    bool hasModuleDecl = false;
};

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
            if (c == '}') templateBraceDepths.pop_back();
            if (scanTemplateChunk()) {
                tokens.push_back({ScanTokKind::TemplateEnd, code.substr(begin, pos - begin), sawNewline});
            } else {
                tokens.push_back({ScanTokKind::LParen, "(", sawNewline});
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
    enum class BraceScope { Normal, FunctionBody, ClassBody };
    std::vector<BraceScope> braceScopes;
    struct ConciseArrowScope { int parenDepth = 0, bracketDepth = 0, braceDepth = 0; };
    std::vector<ConciseArrowScope> conciseArrows;

    int parenDepth = 0, bracketDepth = 0, functionDepth = 0;
    bool pendingClassBody = false;
    const size_t tokCount = tokens.size();

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
            if (k < tokCount && tokens[k].kind == ScanTokKind::Ident) ++k;
            if (k < tokCount && tokens[k].kind == ScanTokKind::LParen) markBodyAfterParams(k);
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

        if (braceScopes.empty() && parenDepth == 0 && bracketDepth == 0 && functionDepth == 0) {
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

    return result;
}

} // namespace

bool hasAwaitStmt(const std::string& code) {
    return scanJsTopLevel(code).hasTopLevelAwait;
}

bool hasImportStmt(const std::string& code) {
    return scanJsTopLevel(code).hasModuleDecl;
}

namespace {

// Appended to a driver script so its END is observable (evalScriptFileJit).
// A property write on globalThis, not a declaration: the name is nothing a
// test could collide with, and it needs no host-globals entry.
constexpr const char* kScriptDoneMarker = "\n;globalThis.__broScriptDone = true;\n";

void resetScriptDone() {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        ev::setProperty(gt.value, "__broScriptDone", ev::fromBool(false));
    }
}

bool scriptDone() {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return true;
    Value v = ev::getProperty(gt.value, "__broScriptDone");
    return ev::isBool(v) && ev::toBool(v);
}

// Fire the rejection events the last drains queued NOW rather than at the
// next frame's task drain, which a run that is about to end never reaches:
// an unhandled rejection left by a script's final turn is still reported
// (and, uncancelled, fails the run). Bounded, since a handler may reject
// again.
void settleRejections() {
    for (int i = 0; i < 8 && rejectionEventsPending(); ++i) {
        flushRejectionEvents();
        if (ev::microtasksPending()) ev::drainMicrotasks();
    }
}

// A headless driver script suspended at a top-level `await` is still
// running: frames are pumped — timers, rAF, host tasks, worker messages, the
// microtask checkpoint, each advancing the virtual clock one frame and kept
// no faster than the wall clock, so a worker or a decode on another thread
// gets the time its reply takes — until the script reaches its end or the
// run has already failed. One that never gets there fails the run: a test
// whose remainder never executed has not passed.
void awaitScriptCompletion(engine::Engine& engine, const std::string& filename) {
    if (engine.displayMode() != engine::DisplayMode::Headless) return;
    if (scriptDone() || hasTestFailure() || engine.hasTestFailure()) return;

    double limitMs = 30000.0;
    if (const char* env = std::getenv("BRO_SCRIPT_SETTLE_MS")) {
        const double v = std::atof(env);
        if (v > 0.0) limitMs = v;
    }
    constexpr double kFrameMs = 1000.0 / 60.0;
    const auto start = std::chrono::steady_clock::now();
    double virtualMs = 0.0;
    while (!scriptDone() && !hasTestFailure() && !engine.hasTestFailure()) {
        const double wallMs = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start).count();
        if (wallMs >= limitMs) break;
        if (virtualMs > wallMs) {
            std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(virtualMs - wallMs));
        }
        engine.advanceTime(kFrameMs);
        virtualMs += kFrameMs;
        pumpBrokitTicks();
        if (ev::microtasksPending()) ev::drainMicrotasks();
        settleRejections();
    }
    if (!scriptDone() && !hasTestFailure() && !engine.hasTestFailure()) {
        LOG_ERROR("evalScriptFileJit: the script's top-level await never settled "
                  "(%.0f ms, %.0f ms of frames) — the rest of %s did not run",
                  limitMs, virtualMs, filename.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
    }
}

// The on-disk code cache (bronze src/eval/code_cache.h, docs/code-cache.md):
// in the per-user cache directory unless BRO_CODE_CACHE=0 turns it off;
// BRO_CODE_CACHE_DIR moves it and BRO_CODE_CACHE_MAX_MB bounds it.
void configureCodeCache(bronze::eval::EvalOptions& opts) {
    static const std::string dir = [] {
        const char* off = std::getenv("BRO_CODE_CACHE");
        if (off && (std::strcmp(off, "0") == 0 || std::strcmp(off, "false") == 0)) return std::string();
        if (const char* d = std::getenv("BRO_CODE_CACHE_DIR"); d && *d) return std::string(d);
        return util::userCacheDir() + "/code-cache";
    }();
    static const uint64_t maxBytes = [] {
        if (const char* mb = std::getenv("BRO_CODE_CACHE_MAX_MB")) {
            const long long v = std::atoll(mb);
            if (v > 0) return static_cast<uint64_t>(v) << 20;
        }
        return uint64_t{512} << 20;
    }();
    opts.codeCacheDir = dir;
    opts.codeCacheMaxBytes = maxBytes;
}

const char* cacheStatusName(bronze::eval::CodeCacheStatus s) {
    switch (s) {
        case bronze::eval::CodeCacheStatus::Hit: return "hit";
        case bronze::eval::CodeCacheStatus::Miss: return "miss";
        default: return "off";
    }
}

// Compiles `code`. In a windowed run the compile goes to another thread
// (bronze compiles against inputs captured here, on the thread that will run
// the program) and this thread keeps the window alive: events pumped, the
// page's static markup drawn with the compile's progress published on <html>
// (Engine::pumpCompileFrame, docs/compile-progress.md). Headless compiles
// inline, which is what keeps a test run deterministic.
// The page's first script, compiling ahead on its own thread
// (startAppScriptCompile) while the engine brings the GPU up. Only the page
// thread touches it.
struct AheadCompile {
    std::string code;  // exactly what is compiled (the async-IIFE form when wrapped)
    std::string filename;
    bool moduleFile = false;
    bronze::eval::EvalOptions opts;  // the options it compiles with, inputs captured
    std::shared_ptr<std::atomic<double>> progress;
    std::shared_ptr<std::atomic<double>> endMs;
    double startMs = -1.0;
    std::future<std::unique_ptr<bronze::eval::CompiledScript>> future;
};
std::unique_ptr<AheadCompile> s_ahead;

// Waits for `future`, keeping a window alive while it does (see below).
std::unique_ptr<bronze::eval::CompiledScript> awaitCompile(
    engine::Engine& engine, std::future<std::unique_ptr<bronze::eval::CompiledScript>>& future,
    const std::shared_ptr<std::atomic<double>>& progress) {
    if (engine.displayMode() == engine::DisplayMode::Windowed && engine.window()) {
        engine.setAppCompiling(true);
        while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            engine.pumpCompileFrame(progress->load(std::memory_order_relaxed));
        }
        engine.setAppCompiling(false);
        engine.setCompileProgress(false, 1.0);
    }
    return future.get();
}

// The ahead-of-time compile of exactly this script, if one is running: taken
// (and its captured inputs copied into `opts`, which the run then binds
// against) or null.
std::unique_ptr<AheadCompile> takeAheadCompile(const bronze::eval::EvalOptions& opts, const std::string& code) {
    if (!s_ahead) return nullptr;
    if (s_ahead->filename != opts.filename || s_ahead->moduleFile != opts.publishEntry ||
        s_ahead->code != code) {
        return nullptr;
    }
    return std::move(s_ahead);
}

std::unique_ptr<bronze::eval::CompiledScript> compileWithPumping(engine::Engine& engine,
                                                                 bronze::eval::EvalOptions& opts,
                                                                 const std::string& code) {
    const auto start = std::chrono::steady_clock::now();
    std::unique_ptr<bronze::eval::CompiledScript> compiled;
    if (auto ahead = takeAheadCompile(opts, code)) {
        // Compiled while the GPU came up (Engine::beginPageCompile): run it
        // against the inputs it was compiled with, and wait out the rest.
        opts.externalModules = ahead->opts.externalModules;
        opts.externalModulesCaptured = ahead->opts.externalModulesCaptured;
        opts.nativeManifestJson = ahead->opts.nativeManifestJson;
        const double waitStart = engine::sinceStartMs();
        compiled = awaitCompile(engine, ahead->future, ahead->progress);
        const double waited = engine::sinceStartMs() - waitStart;
        const double endMs = ahead->endMs->load(std::memory_order_relaxed);
        const char* cache = compiled ? cacheStatusName(compiled->cacheStatus) : "off";
        engine::notePageCompile(ahead->startMs, endMs, waited, cache);
        if (compiled && compiled->cacheStatus != bronze::eval::CodeCacheStatus::Off) {
            LOG_INFO("compiled %s in %.0f ms (code cache %s%s%s), while the GPU came up; waited %.0f ms for it",
                     opts.filename.c_str(), endMs - ahead->startMs, cache,
                     compiled->cacheNote.empty() ? "" : ": ", compiled->cacheNote.c_str(), waited);
        }
        return compiled;
    }
    if (engine.displayMode() == engine::DisplayMode::Windowed && engine.window()) {
        // Captured fresh for every compile: a retry after a failed run sees
        // the modules that run published.
        opts.externalModules.clear();
        opts.externalModulesCaptured = false;
        opts.nativeManifestJson.reset();
        bronze::eval::captureThreadInputs(opts);
        auto progress = std::make_shared<std::atomic<double>>(0.0);
        opts.onProgress = [progress](const bronze::eval::CompileProgress& p) {
            progress->store(p.fraction, std::memory_order_relaxed);
        };
        auto future = std::async(std::launch::async, [&opts, &code] {
            return bronze::eval::compileScript(code, opts);
        });
        compiled = awaitCompile(engine, future, progress);
        opts.onProgress = nullptr;
    } else {
        compiled = bronze::eval::compileScript(code, opts);
    }
    if (compiled && compiled->cacheStatus != bronze::eval::CodeCacheStatus::Off) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        LOG_INFO("compiled %s in %.0f ms (code cache %s%s%s)", opts.filename.c_str(), ms,
                 cacheStatusName(compiled->cacheStatus), compiled->cacheNote.empty() ? "" : ": ",
                 compiled->cacheNote.c_str());
    }
    return compiled;
}

}  // namespace

bronze::eval::EvalOptions scriptEvalOptions(engine::Engine& engine, const std::string& filename,
                                            bool moduleFile) {
    bronze::eval::EvalOptions opts;
    opts.filename = filename.empty() ? "<eval>" : filename;
    // Read off the registry the install filled, not from a list kept beside
    // it: what is registered is exactly what the compile admits.
    opts.hostGlobals = registeredHostGlobals();
    opts.moduleRoots = moduleRootsFor(engine);
    opts.entryResolvesAs = entryResolvesAsFor(engine, filename);
    opts.retainSource = true;
    opts.pinsPath = discoverPinsPath(engine, filename);
    opts.censusOutPath = discoverCensusOutPath(engine, filename);
    // One module map per realm. The page is one compilation unit and a headless
    // driver script is another, so without this a test's `import "/app/lib/x.js"`
    // compiles the app's module into its OWN unit and evaluates it a second
    // time — a second instance of state the page already holds. Publishing here
    // is what makes the page's instances the ones a later unit binds
    // (bronze: runtime/module_registry.h).
    opts.moduleRegistry = true;
    // A `<script type="module" src>` is itself a module instance: published
    // too, so a driver's `import "/app/main.js"` binds it rather than booting
    // the app a second time. Inline script text and driver scripts are not.
    opts.publishEntry = moduleFile && !filename.empty();
    configureCodeCache(opts);
    return opts;
}

std::string scriptExecCode(const std::string& code, const std::string& filename) {
    if (hasAwaitStmt(code) && !hasImportStmt(code)) return wrapAsyncIife(code, filename);
    return code;
}

bool startAppScriptCompile(engine::Engine& engine, const std::string& code, const std::string& filename,
                           bool moduleFile) {
    if (isJitDisabled() || s_ahead || code.empty()) return false;
    {
        HostEvalScope evalScope;
        if (!isWebHostGlobalsInstalled()) {
            const double at = engine::sinceStartMs();
            installWebHostGlobals(engine);
            engine::notePageGlobals(at, engine::sinceStartMs() - at);
        }
    }
    auto ahead = std::make_unique<AheadCompile>();
    ahead->code = scriptExecCode(code, filename);
    ahead->filename = filename.empty() ? "<eval>" : filename;
    ahead->moduleFile = moduleFile && !filename.empty();
    ahead->opts = scriptEvalOptions(engine, filename, moduleFile);
    // A current ahead-of-time module is the compile, already done.
    if (loadAheadAotUnit(engine, ahead->code, ahead->opts)) return false;
    // Everything the compile reads from this thread's runtime, captured here:
    // the compile itself then runs on its own thread (bronze compileScript).
    bronze::eval::captureThreadInputs(ahead->opts);
    ahead->progress = std::make_shared<std::atomic<double>>(0.0);
    ahead->endMs = std::make_shared<std::atomic<double>>(-1.0);
    ahead->opts.onProgress = [progress = ahead->progress](const bronze::eval::CompileProgress& p) {
        progress->store(p.fraction, std::memory_order_relaxed);
    };
    ahead->startMs = engine::sinceStartMs();
    // The task owns copies of what it reads: the entry may be dropped (the
    // page changed its scripts) while it runs, and is then joined.
    ahead->future = std::async(std::launch::async,
                               [code = ahead->code, opts = ahead->opts, endMs = ahead->endMs] {
        auto compiled = bronze::eval::compileScript(code, opts);
        endMs->store(engine::sinceStartMs(), std::memory_order_relaxed);
        return compiled;
    });
    s_ahead = std::move(ahead);
    return true;
}

void discardAppScriptCompile() {
    discardAheadAotUnit();
    if (!s_ahead) return;
    if (s_ahead->future.valid()) s_ahead->future.wait();
    s_ahead.reset();
}

void applyCodeCacheOptions(bronze::eval::EvalOptions& opts) {
    configureCodeCache(opts);
}

std::string wrapAsyncIife(const std::string& code, const std::string& filename) {
    // The filename as a JS string literal: backslashes (Windows paths) and
    // quotes escaped, nothing else can appear in a path the OS opened.
    std::string where;
    for (char c : filename) {
        if (c == '\\' || c == '\'') where += '\\';
        where += c;
    }
    return "(async () => {\n" + code +
           "\n})().catch(err => {"
           " const text = err && typeof err.stack === 'string' && err.stack ? err.stack"
           " : err && typeof err.message === 'string' ? ((err.name || 'Error') + ': ' + err.message)"
           " : String(err);"
           " const where = '" + where + "';"
           " const report = 'Unhandled error: ' + text + (where ? ' (in ' + where + ')' : '');"
           " if (typeof assert === 'function') assert(false, report);"
           " else console.error(report); });\n";
}

bronze::embed::CallResult evalScriptJitResult(engine::Engine& engine, const std::string& code,
                                              const std::string& filename,
                                              bronze::embed::ModuleHandle* moduleHandleOut,
                                              bool moduleFile) {
    HostEvalScope evalScope;
    if (!isWebHostGlobalsInstalled()) {
        installWebHostGlobals(engine);
    }

    bronze::eval::EvalOptions opts = scriptEvalOptions(engine, filename, moduleFile);
    opts.moduleHandleOut = moduleHandleOut;
    const std::string execCode = scriptExecCode(code, filename);

    // An installed app's executable runs the unit's ahead-of-time module
    // while it is current (app_aot.h); anything else compiles here.
    bronze::embed::CallResult res;
    if (auto aot = takeAotUnit(engine, execCode, opts)) {
        res = bronze::eval::runModule(*aot, opts);
    } else {
        auto compiled = compileWithPumping(engine, opts, execCode);
        res = bronze::eval::runCompiledScript(std::move(compiled), opts);
    }

    if (res.thrown && execCode == code && !hasImportStmt(code)) {
        // toUtf8 allocates, so the thrown value is rooted across it and read
        // back: the caller reports res.value, which must not be stale.
        bronze::embed::Persistent thrownRoot(res.value);
        std::string errStr = bronze::embed::toUtf8(thrownRoot.get());
        res.value = thrownRoot.get();
        if (errStr.find("await") != std::string::npos) {
            // The failed attempt is off the stack and superseded: its handle
            // is retired here so the one written below is the run's only one.
            if (moduleHandleOut && *moduleHandleOut) bronze::embed::unloadModule(*moduleHandleOut);
            auto retryCompiled = compileWithPumping(engine, opts, wrapAsyncIife(code, filename));
            res = bronze::eval::runCompiledScript(std::move(retryCompiled), opts);
        }
    }
    return res;
}

bool evalScriptJit(engine::Engine& engine, const std::string& code, const std::string& filename,
                   bronze::embed::ModuleHandle* moduleHandleOut, bool moduleFile) {
    HostEvalScope evalScope;
    auto res = evalScriptJitResult(engine, code, filename, moduleHandleOut, moduleFile);
    if (!reportCallResult(engine, res, "evalScriptJit", filename)) {
        return false;
    }

    drainMicrotasksAndLocalFetches();
    settleRejections();
    std::fflush(stdout);

    if (hasTestFailure() || engine.hasTestFailure()) {
        return false;
    }
    return true;
}

bool evalScriptFileJit(engine::Engine& engine, const std::string& filePath) {
    HostEvalScope evalScope;
    std::error_code ec;
    std::filesystem::path p(filePath);
    if (!std::filesystem::exists(p, ec)) {
        LOG_ERROR("evalScriptFileJit: file does not exist: %s", filePath.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    if (!isWebHostGlobalsInstalled()) {
        installWebHostGlobals(engine);
    }

    auto absPath = std::filesystem::absolute(p, ec);
    // require('./x') in the script resolves against the script's directory,
    // as brokit's own runner and Node have it (docs/brokit-api.js), not the
    // cwd: for the script's run and what it awaits to its end.
    brokit::api::RequireDirGuard requireDir(absPath.parent_path());

    std::string content;
    {
        std::ifstream ifs(absPath, std::ios::binary);
        if (ifs.is_open()) {
            content.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        }
    }

    bronze::eval::EvalOptions opts;
    opts.filename = absPath.string();
    opts.hostGlobals = registeredHostGlobals();
    opts.moduleRoots = moduleRootsFor(engine);
    opts.entryResolvesAs = absPath;
    opts.retainSource = true;
    opts.pinsPath = discoverPinsPath(engine, absPath);
    opts.censusOutPath = discoverCensusOutPath(engine, absPath);
    // The consuming half of the same seam: a specifier that resolves to a file
    // the page already evaluated binds that instance instead of compiling a
    // second copy of it into this unit. The driver's OWN file is the entry and
    // is never published, so running the same driver twice runs it twice.
    opts.moduleRegistry = true;
    configureCodeCache(opts);

    // The script's last statement records that it RAN to its end. A script
    // suspended at a top-level `await` (the async-IIFE form, or a module's
    // native TLA) returns from its top level long before that, and without
    // the marker a run would stop there: the rest of the test never executed
    // and the run still read as a pass.
    const std::string source = content + kScriptDoneMarker;
    resetScriptDone();

    bronze::embed::CallResult res;
    if (hasAwaitStmt(content) && !hasImportStmt(content)) {
        auto compiled = compileWithPumping(engine, opts, wrapAsyncIife(source, absPath.string()));
        res = bronze::eval::runCompiledScript(std::move(compiled), opts);
    } else {
        auto compiled = compileWithPumping(engine, opts, source);
        res = bronze::eval::runCompiledScript(std::move(compiled), opts);
        if (res.thrown) {
            // Rooted across the allocating toUtf8, as in evalScriptJitResult.
            bronze::embed::Persistent thrownRoot(res.value);
            std::string errStr = bronze::embed::toUtf8(thrownRoot.get());
            res.value = thrownRoot.get();
            if (errStr.find("await") != std::string::npos && !hasImportStmt(content)) {
                auto retryCompiled = compileWithPumping(engine, opts, wrapAsyncIife(source, absPath.string()));
                res = bronze::eval::runCompiledScript(std::move(retryCompiled), opts);
            }
        }
    }

    if (!reportCallResult(engine, res, "evalScriptFileJit", absPath.string())) {
        return false;
    }

    drainMicrotasksAndLocalFetches();
    awaitScriptCompletion(engine, absPath.string());
    settleRejections();
    std::fflush(stdout);

    if (hasTestFailure() || engine.hasTestFailure()) {
        return false;
    }
    return true;
}

} // namespace bro::bronze_host
