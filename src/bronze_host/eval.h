#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace bronze::modules {
struct ModuleRoot;
}

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

struct Compartment;

/// How a compile's top level binds. `Module`: a page or iframe `<script
/// type="module">` (and internal expression helpers), whose top-level
/// declarations stay its own bindings (`publishTopLevel = false`,
/// `classicScript = false`). `Unit`: a host-evaluated unit whose declarations
/// stay its own bindings while it runs and are copied onto `globalThis` once
/// it has finished (`EvalOptions::publishTopLevel = true`: headless driver
/// scripts, `-e`, `bro-ctl eval`, `eval()`). `ClassicScript`: one of HTML's
/// classic scripts, whose top-level `var` and function declarations are
/// properties of the global object made as the script starts, whose top-level
/// `this` is the global object, and which is sloppy unless it says "use strict"
/// (bronze `EvalOptions::classicScript`); `let`, `const` and `class` stay the
/// unit's. `ClassicScripts`: several classic scripts joined into one unit, each
/// entered by its `classicScriptMarker` statement
/// (`Engine::combineClassicScripts`), so a function of a later script is not
/// yet defined while an earlier runs and each script's own `"use strict"`
/// directive prologue is honoured.
enum class ScriptGoal { Module, Unit, ClassicScript, ClassicScripts };
/// A page unit's goal: the classic unit's (its scripts listed) or a module's.
inline ScriptGoal pageUnitGoal(const std::vector<std::string>& classicScripts) {
    return classicScripts.empty() ? ScriptGoal::Module : ScriptGoal::ClassicScripts;
}

/// Module roots for a compile that runs against `engine`'s app.
std::vector<bronze::modules::ModuleRoot> moduleRootsFor(const engine::Engine& engine);

/// Where script text handed to evalScript lives for imports.
std::string entryResolvesAsFor(const engine::Engine& engine, const std::string& filename);

/// Compile JS code in process (bronze's evaluator) and run it on `engine`.
/// `filename` is the document the code came from (an app's index.html for its
/// `<script>` text): its relative imports resolve from there. Empty means
/// "no document", and they resolve from the app dir instead.
/// `goal`: ClassicScript for a classic <script> of a frame.
bool evalScript(engine::Engine& engine, const std::string& code,
                const std::string& filename = {}, ScriptGoal goal = ScriptGoal::Unit);

/// evalScript for the app's OWN scripts — the `<script>` text of its
/// index.html. The run is bracketed as a module load and the handle recorded
/// on `engine` (Engine::addAppModuleHandle), so a reload can unload it: the
/// roots the program registered stop being roots and the old realm's heap
/// dies with it, instead of every reload leaking the previous app's globals,
/// closures and environments for the life of the process. A driver script or
/// an `eval()` goes through evalScript and is nobody's to unload.
/// `moduleFile`: `code` is the text of the module file `filename` (a
/// `<script type="module" src>`), whose instance is then what a later
/// `import` of that file binds — a driver script importing the app's entry
/// gets the running app, not a second boot of it.
/// `goal`: ClassicScripts for the page's joined classic unit.
bool evalAppScript(engine::Engine& engine, const std::string& code,
                   const std::string& filename, bool moduleFile, ScriptGoal goal);

/// evalScript for a child compartment's sub-document (`<appview>` / `<iframe>`),
/// tracking its loaded module handle on `comp` so it can be unloaded on
/// compartment teardown and allowing its own AOT directory to load/harvest.
bool evalSubDocScript(engine::Engine& engine, Compartment& comp, const std::string& code,
                      const std::string& filename, bool moduleFile, ScriptGoal goal);

/// A page's classic scripts run as one unit (Engine::pageScriptUnits) whose
/// text sets a marker as it enters each script, `classicScriptMarker(i)`, and
/// once more past the last (i = the script count). A unit whose run threw is
/// then resumed after the script that threw, as the web runs each script as a
/// task of its own: the marker says which one that was, and -1 (never set)
/// that the unit did not compile. Cleared before the run, taken after it.
std::string classicScriptMarker(size_t index);
/// The global property the marker sets (bronze EvalOptions::classicScriptMarker).
const char* classicScriptMarkerName();
void clearClassicScriptMarker();
int takeClassicScriptMarker();
/// The page ordinal of the classic script running now (the last marker the
/// unit set), or -1 outside a classic unit. Read without clearing it.
int currentClassicScript();
/// Records the document's classic <script> elements, in document order, as
/// the page's classic scripts start (Engine::runClassicScripts): the
/// elements document.currentScript names by currentClassicScript()'s
/// ordinal. A script element inserted while they run is not one of them
/// (dom_document.cpp).
void snapshotClassicScripts();
/// A statement firing `type` ("load" or "error") at the classic script
/// running now (currentClassicScript's element): a trusted Event that neither
/// bubbles nor cancels, as HTML fires at a script element whose fetch
/// succeeded (after it ran) or failed (in place of running it). The unit's
/// text carries it (Engine::combineClassicScripts); installClassicScriptEvents
/// puts the function it calls on the realm's global.
std::string classicScriptEventCall(const char* type);
void installClassicScriptEvents();

/// Compile a JS file in process and run it on `engine`.
bool evalScriptFile(engine::Engine& engine, const std::string& filePath);

/// Install dynamic evaluation and function hooks (eval(), new Function())
/// into the Bronze runtime.
void installDynamicHooks(engine::Engine& engine);

} // namespace bro::bronze_host
