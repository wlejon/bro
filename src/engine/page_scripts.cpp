// The page's scripts as units (Engine::pageScriptUnits): the classic
// scripts combined into one, each module its own, and how a classic unit
// resumes after a script that threw (Engine::runClassicScripts).

#include "engine/engine.h"
#include "engine/app_loader.h"
#include "bronze_host/eval.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace bro::engine {

// One text per classic script, an empty one included, so a script's index
// here is its ordinal among the page's classic <script>s (the marker
// document.currentScript reads).
std::vector<std::string> Engine::classicScriptTexts() const {
    std::vector<std::string> texts;
    classicScriptFetch_.clear();
    for (const auto& script : manifest_.scripts) {
        if (script.isModule || script.isLua) continue;
        if (script.isInline()) {
            texts.push_back(script.code);
            classicScriptFetch_.push_back(kScriptInline);
            continue;
        }
        std::optional<std::string> body = AppLoader::readFile(script.path);
        classicScriptFetch_.push_back(body ? kScriptFetched : kScriptFailed);
        texts.push_back(body ? std::move(*body) : std::string());
    }
    return texts;
}

// The page's classic scripts as one unit, every script's top-level bindings
// in one scope as they are in the web's one global scope. Each script is
// entered by a marker statement (bronze_host::classicScriptMarker) on a line
// the unit already has, so line numbers stay the scripts' own and a throw is
// known to be the script's it came out of (runClassicScripts). Markers count
// from `first`, the page ordinal of texts[0], so a resumed unit's scripts keep
// their numbers.
//
// An external script fires `load` at its element after it ran, on the line of
// the next marker, and one whose fetch failed (missing, refused, unreachable)
// fires `error` in place of its text, the page going on with the next script
// as after a network error on the web.
std::string Engine::combineClassicScripts(const std::vector<std::string>& texts, size_t first) const {
    auto fetch = [this](size_t ordinal) {
        return ordinal < classicScriptFetch_.size() ? classicScriptFetch_[ordinal] : kScriptInline;
    };
    auto loaded = [&](size_t ordinal) {
        return fetch(ordinal) == kScriptFetched ? bronze_host::classicScriptEventCall("load") : std::string();
    };
    std::string combined;
    for (size_t i = 0; i < texts.size(); ++i) {
        combined += i == 0 ? std::string() : "\n;" + loaded(first + i - 1);
        combined += bronze_host::classicScriptMarker(first + i);
        if (i > 0) combined += "\n";
        if (fetch(first + i) == kScriptFailed) combined += bronze_host::classicScriptEventCall("error");
        combined += texts[i];
    }
    if (!texts.empty()) {
        combined += "\n;" + loaded(first + texts.size() - 1) +
                    bronze_host::classicScriptMarker(first + texts.size()) + "\n";
    }
    return combined;
}

// A classic script's uncaught exception ends that script only: the error has
// been reported (window.onerror, the error event, the log) and the scripts
// after it run, as a unit of their own. A unit that did not compile runs its
// scripts one by one, so the one with the syntax error is the one that is
// lost.
void Engine::runClassicScripts(const std::vector<std::string>& texts, const std::string& code,
                               const std::string& name, size_t first) {
    if (first == 0) bronze_host::snapshotClassicScripts();
    bronze_host::clearClassicScriptMarker();
    if (bronze_host::evalAppScript(*this, code, name, false)) {
        bronze_host::takeClassicScriptMarker();
        return;
    }
    setTestFailure(true);
    const int marked = bronze_host::takeClassicScriptMarker();
    const int reached = marked < 0 ? -1 : marked - static_cast<int>(first);
    if (reached >= static_cast<int>(texts.size())) return;  // ran to its end
    if (reached < 0) {
        if (texts.size() < 2) return;
        for (size_t i = 0; i < texts.size(); ++i) {
            std::vector<std::string> one{texts[i]};
            runClassicScripts(one, combineClassicScripts(one, first + i), name, first + i);
        }
        return;
    }
    std::vector<std::string> rest(texts.begin() + reached + 1, texts.end());
    if (rest.empty()) return;
    const size_t restFirst = first + static_cast<size_t>(reached) + 1;
    // Blank lines first, so the rest's scripts keep the lines they have in
    // this unit and an error names the same line either way.
    const size_t marker = code.find(bronze_host::classicScriptMarker(restFirst));
    const size_t lines = marker == std::string::npos
        ? 0 : static_cast<size_t>(std::count(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(marker), '\n')) + 1;
    runClassicScripts(rest, std::string(lines, '\n') + combineClassicScripts(rest, restFirst), name, restFirst);
}

// Module scripts are deferred on the web — they run after the classic ones, in
// document order — and each is its own module with its own URL: an external
// one is named for its file, so import.meta.url and its relative imports
// resolve from where it lives rather than from index.html; an inline one is
// named for the page, whose URL is its base. An external module script IS the
// module instance for its file: it is published under its path, so a later
// `import` of that file — from a headless driver script, or another module
// script — binds the instance that ran rather than running the file again. Two
// tags with one src are one module, evaluated once (HTML's module map).
std::vector<Engine::PageScriptUnit> Engine::pageScriptUnits() const {
    std::vector<PageScriptUnit> units;
    if (displayMode_ == DisplayMode::Server && !manifest_.htmlPath.empty()) return units;
    std::vector<std::string> classic = classicScriptTexts();
    if (!classic.empty()) {
        std::string unitName = !manifest_.htmlPath.empty()
            ? manifest_.htmlPath
            : (!manifest_.scripts.empty() ? manifest_.scripts.front().path : appDir_);
        std::string code = combineClassicScripts(classic);
        units.push_back({std::move(code), unitName, false, std::move(classic)});
    }
    std::vector<std::string> moduleFiles;
    for (const auto& script : manifest_.scripts) {
        if (!script.isModule && !script.isLua) continue;
        if (!script.isInline()) {
            if (std::find(moduleFiles.begin(), moduleFiles.end(), script.path) != moduleFiles.end()) continue;
            moduleFiles.push_back(script.path);
        }
        std::string code = script.isInline() ? script.code : AppLoader::loadFile(script.path);
        if (code.empty()) continue;
        std::string unitName = script.isInline()
            ? (script.isLua ? (manifest_.htmlPath + ".lua") : manifest_.htmlPath)
            : script.path;
        units.push_back({std::move(code), std::move(unitName), !script.isInline() && !script.isLua});
    }
    return units;
}

} // namespace bro::engine
