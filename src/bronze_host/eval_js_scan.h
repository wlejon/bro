#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace bro::bronze_host {

struct TopLevelVarExport {
    size_t insertOffset = 0;
    std::vector<std::string> names;
};

struct JsTopLevelScan {
    bool hasTopLevelAwait = false;
    bool hasModuleDecl = false;
    size_t directiveEndOffset = 0;
    std::vector<std::string> topLevelFunctionDecls;
    std::vector<TopLevelVarExport> topLevelVarExports;
};

/// Single-pass lexical and structural scan of JavaScript source for top-level
/// `await`, ES module `import`/`export` declarations, and top-level classic
/// script `function`/`var` bindings.
JsTopLevelScan scanJsTopLevel(std::string_view code);

/// Whether `code` has a top-level `await` (function depth 0, outside comments,
/// strings, template literals, regexes, and nested function/arrow/method/class
/// bodies).
bool hasAwaitStmt(const std::string& code);

/// Whether `code` has a top-level ES module `import` or `export` declaration
/// (outside comments/strings/templates/regexes, excluding dynamic `import(...)`
/// calls and `import.meta`).
bool hasImportStmt(const std::string& code);

/// For classic (non-module) scripts: hoists top-level `function <fn>` exports
/// that shadow replaceable host globals to `{globalThis.<fn> = <fn>;}` at the
/// start of the script (after any directive prologue) and synchronizes
/// top-level `var <id> = ...` declarations with `globalThis.<id>` (coercing
/// `var name = ...` through the `window.name` setter) immediately after each
/// statement, without adding newlines or altering line numbers.
std::string prepareClassicScriptGlobals(const std::string& code);

} // namespace bro::bronze_host
