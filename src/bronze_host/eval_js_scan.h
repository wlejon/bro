#pragma once

#include <string>
#include <string_view>

namespace bro::bronze_host {

struct JsTopLevelScan {
    bool hasTopLevelAwait = false;
    bool hasModuleDecl = false;
};

/// Single-pass lexical and structural scan of JavaScript source for top-level
/// `await` and ES module `import`/`export` declarations.
JsTopLevelScan scanJsTopLevel(std::string_view code);

/// Whether `code` has a top-level `await` (function depth 0, outside comments,
/// strings, template literals, regexes, and nested function/arrow/method/class
/// bodies).
bool hasAwaitStmt(const std::string& code);

/// Whether `code` has a top-level ES module `import` or `export` declaration
/// (outside comments/strings/templates/regexes, excluding dynamic `import(...)`
/// calls and `import.meta`).
bool hasImportStmt(const std::string& code);

} // namespace bro::bronze_host
