// The reload preflight (engine/source_preflight.h): a tree caught half-way
// through a restructure must read as incomplete, so the source watcher keeps
// the running page instead of tearing it down for a reload that cannot work.
// The fixtures replay the shape of the failure that motivated it: a git
// operation that rewrote shell.js to import a renamed class before the module
// exporting it had been rewritten.

#include "engine/source_preflight.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <chrono>

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

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("bro_source_preflight_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
