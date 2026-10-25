// The reload preflight (engine/source_preflight.h): a tree caught half-way
// through a restructure must read as incomplete, so the source watcher keeps
// the running page instead of tearing it down for a reload that cannot work.
// The fixtures replay the shape of the failure that motivated it: a git
// operation that rewrote shell.js to import a renamed class before the module
// exporting it had been rewritten.

#include "engine/source_preflight.h"
#include "bronze_host/eval_js_scan.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <chrono>

namespace bro::bronze_host {
bool isBaselineHostGlobalName(const std::string& name) {
    return name == "Symbol" || name == "fetch";
}
} // namespace bro::bronze_host

namespace fs = std::filesystem;

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what, const std::string& detail = {}) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s%s%s\n", what, detail.empty() ? "" : " -- ", detail.c_str());
    }
}

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

const char* kIndex =
    "<!DOCTYPE html><html><head>"
    "<link rel=\"stylesheet\" href=\"css/base.css\">"
    "</head><body><div id=\"bar\"></div>"
    "<script type=\"module\" src=\"js/shell.js\"></script>"
    "</body></html>";

void writeConsistentTree(const fs::path& dir) {
    fs::remove_all(dir);
    write(dir / "index.html", kIndex);
    write(dir / "css/base.css", "body { margin: 0 }\n");
    write(dir / "js/shell.js",
          "// import { Gone } from './gone.js';  (a comment is not an import)\n"
          "import { NotificationCenter } from './notify.js';\n"
          "import Bar, { BAR_HEIGHT as H } from './bar.js';\n"
          "import * as util from './util.js';\n"
          "import './side_effect.js';\n"
          "import { thing } from 'bare-package';\n"
          "const later = import('./lazy_missing.js');\n"
          "export class Shell { constructor() { this.n = new NotificationCenter(); } }\n");
    write(dir / "js/notify.js",
          "import { esc } from './util.js';\n"
          "export class NotificationCenter {}\n");
    write(dir / "js/bar.js",
          "export const BAR_HEIGHT = 32;\n"
          "export default class Bar {}\n");
    write(dir / "js/util.js",
          "function esc(s) { return s; }\n"
          "const one = 1, two = 2;\n"
          "export { esc, one as ONE };\n"
          "export * from './more.js';\n");
    write(dir / "js/more.js", "export const more = 1;\n");
    write(dir / "js/side_effect.js", "globalThis.x = 1;\n");
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() /
                         ("bro_preflight_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    using bro::engine::checkAppSourceConsistency;

    writeConsistentTree(dir);
    std::string why = checkAppSourceConsistency(dir.string());
    check(why.empty(), "a complete tree is consistent", why);

    // shell.js rewritten first: it imports a class notify.js does not export yet.
    write(dir / "js/notify.js", "export class NotificationController {}\n");
    why = checkAppSourceConsistency(dir.string());
    check(why.find("NotificationCenter") != std::string::npos &&
              why.find("js/notify.js") != std::string::npos,
          "a renamed export reads as incomplete", why);

    // A module the graph imports has not been written yet.
    writeConsistentTree(dir);
    fs::remove(dir / "js/bar.js");
    why = checkAppSourceConsistency(dir.string());
    check(why.find("./bar.js") != std::string::npos, "a missing import reads as incomplete", why);

    // A transitive import (notify.js -> util.js) missing.
    writeConsistentTree(dir);
    write(dir / "js/notify.js",
          "import { esc } from './helpers/esc.js';\nexport class NotificationCenter {}\n");
    why = checkAppSourceConsistency(dir.string());
    check(why.find("helpers/esc.js") != std::string::npos, "a missing transitive import", why);

    // index.html removed mid-checkout, or truncated to nothing.
    writeConsistentTree(dir);
    fs::remove(dir / "index.html");
    why = checkAppSourceConsistency(dir.string());
    check(why.find("index.html") != std::string::npos, "a missing index.html", why);
    write(dir / "index.html", "");
    why = checkAppSourceConsistency(dir.string());
    check(why.find("index.html") != std::string::npos, "an empty index.html", why);

    // The page names a script that is not there yet. (A stylesheet is not
    // checked: a page without it still runs, as it does on a cold start.)
    writeConsistentTree(dir);
    fs::remove(dir / "css/base.css");
    why = checkAppSourceConsistency(dir.string());
    check(why.empty(), "a missing stylesheet does not hold a reload back", why);
    writeConsistentTree(dir);
    fs::remove(dir / "js/shell.js");
    why = checkAppSourceConsistency(dir.string());
    check(why.find("js/shell.js") != std::string::npos, "a missing module script", why);

    // Forms the scan cannot be sure about are given the benefit of the doubt:
    // a multi-declarator export and destructuring.
    writeConsistentTree(dir);
    write(dir / "js/bar.js",
          "export const BAR_HEIGHT = 32, OTHER = 1;\n"
          "export default class Bar {}\n");
    write(dir / "js/notify.js",
          "export const { NotificationCenter } = globalThis.__lib;\n");
    why = checkAppSourceConsistency(dir.string());
    check(why.empty(), "uncertain export forms count as present", why);

    // TypeScript (.ts / .mts) module graph with type-only imports/exports,
    // generic type annotations, and .js specifiers resolving to .ts files.
    fs::remove_all(dir);
    write(dir / "index.html",
          "<!DOCTYPE html><html><body>"
          "<script type=\"module\" src=\"ts/main.ts\"></script>"
          "</body></html>");
    write(dir / "ts/main.ts",
          "import type { MissingRuntimeExport } from './types.ts';\n"
          "import { type Config, createApp, Mode } from './app.js';\n"
          "const app = createApp({ mode: Mode.Fast });\n");
    write(dir / "ts/types.ts",
          "export interface MissingRuntimeExport { id: number; }\n");
    write(dir / "ts/app.ts",
          "export type Config = { mode: Mode };\n"
          "export enum Mode { Fast, Safe }\n"
          "export const registry: Map<string, number> = new Map();\n"
          "export function createApp<T extends Config>(cfg: T): { ok: boolean } {\n"
          "    return { ok: true };\n"
          "}\n");
    why = checkAppSourceConsistency(dir.string());
    check(why.empty(), "TypeScript module tree with type imports and .js->.ts resolution is consistent", why);

    // Missing runtime export in .ts module is still caught.
    write(dir / "ts/app.ts",
          "export type Config = { mode: number };\n"
          "export function createApp(cfg: Config): void {}\n");
    why = checkAppSourceConsistency(dir.string());
    check(why.find("Mode") != std::string::npos && why.find("ts/app.ts") != std::string::npos,
          "missing runtime export from .ts module is caught", why);

    // Script-entry app with main.ts (no index.html).
    fs::remove_all(dir);
    write(dir / "main.ts",
          "import { helper } from './lib';\n"
          "helper();\n");
    write(dir / "lib.ts",
          "export function helper(): number { return 42; }\n");
    why = checkAppSourceConsistency(dir.string());
    check(why.empty(), "main.ts entry app without index.html is consistent", why);

    // Lua (.lua) script in index.html and main.lua script-entry app.
    fs::remove_all(dir);
    write(dir / "index.html",
          "<!DOCTYPE html><html><body>"
          "<script src=\"main.lua\"></script>"
          "</body></html>");
    write(dir / "main.lua",
          "local x = (1 << 60) + 42\n"
          "app_lua_ready = true\n");
    why = checkAppSourceConsistency(dir.string());
    check(why.empty(), "index.html with <script src=\"main.lua\"> is consistent", why);

    fs::remove_all(dir);
    write(dir / "main.lua",
          "print('standalone main.lua app')\n");
    why = checkAppSourceConsistency(dir.string());
    check(why.empty(), "main.lua entry app without index.html is consistent", why);

    // eval_js_scan TypeScript top-level scanning checks.
    {
        using bro::bronze_host::scanJsTopLevel;
        using bro::bronze_host::prepareClassicScriptGlobals;

        auto s1 = scanJsTopLevel(
            "interface Box<T> { await?: Promise<T>; get(): T; }\n"
            "type Mapper<A, B> = (a: A) => { out: B };\n"
            "function Symbol<T extends { a: number }>(x: T): { ok: boolean } {\n"
            "    return { ok: true };\n"
            "}\n"
            "var fetch: Map<string, number> = new Map(), name!: string = 'app', uninit: number;\n"
            "const arrow = (x: number): { y: number } => { return { y: x }; };\n"
        );
        check(!s1.hasModuleDecl, "TS classic script has no module decl");
        check(!s1.hasTopLevelAwait, "TS interface await property is not top-level await");
        check(s1.topLevelFunctionDecls.size() == 1 && s1.topLevelFunctionDecls[0] == "Symbol",
              "TS generic function with object return type is detected as top-level function decl");
        check(s1.topLevelVarExports.size() == 1 && s1.topLevelVarExports[0].names.size() == 2 &&
                  s1.topLevelVarExports[0].names[0] == "fetch" &&
                  s1.topLevelVarExports[0].names[1] == "name",
              "TS top-level var with generic and non-null type annotations records initialized names");

        std::string prep = prepareClassicScriptGlobals(
            "function Symbol<T>(x: T): { ok: boolean } { return { ok: true }; }\n"
            "var name: string = 'hello';\n"
        );
        check(prep.find("globalThis.Symbol = Symbol;") != std::string::npos &&
                  prep.find("globalThis.name = name;") != std::string::npos,
              "prepareClassicScriptGlobals instruments TS function and var declarations");

        auto s2 = scanJsTopLevel("export interface Foo { x: number }\n");
        check(s2.hasModuleDecl, "export interface marks script as module");
        auto s3 = scanJsTopLevel("export type Bar = string | number;\n");
        check(s3.hasModuleDecl, "export type marks script as module");
        auto s4 = scanJsTopLevel("export enum Kind { A, B }\n");
        check(s4.hasModuleDecl, "export enum marks script as module");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("bro_source_preflight_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
