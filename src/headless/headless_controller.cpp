#include "headless/headless_controller.h"

#include "engine/engine.h"
#include "engine/system_overlay.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/node.h"
#include "util/log.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>

namespace bro::headless {

HeadlessController::HeadlessController(engine::Engine& engine)
    : engine_(engine) {}

bool HeadlessController::processCommand(const std::string& line) {
    // Trim
    std::string cmd = line;
    while (!cmd.empty() && (cmd.front() == ' ' || cmd.front() == '\t')) cmd.erase(cmd.begin());
    while (!cmd.empty() && (cmd.back() == ' ' || cmd.back() == '\t' ||
                             cmd.back() == '\r' || cmd.back() == '\n')) cmd.pop_back();

    if (cmd.empty() || cmd[0] == '#') return true; // comment or blank

    if (cmd == "quit" || cmd == "exit") return false;

    if (cmd == "dump") {
        auto* doc = engine_.document();
        std::string html = (doc && doc->documentElement()) ? doc->documentElement()->innerHTML() : "";
        std::cout << html << "\n";
        lastDump_ = html;
        return true;
    }

    if (cmd.substr(0, 5) == "dump ") {
        std::string selector = cmd.substr(5);
        auto* el = engine_.querySelector(selector);
        if (!el) {
            std::cout << "(not found: " << selector << ")\n";
        } else {
            std::string tag = el->tagName();
            std::transform(tag.begin(), tag.end(), tag.begin(), ::tolower);
            std::string result = "<" + tag;
            std::string id = el->id();
            if (!id.empty()) result += " id=\"" + id + "\"";
            result += ">" + el->innerHTML() + "</" + tag + ">";
            std::cout << result << "\n";
        }
        return true;
    }

    if (cmd == "diff") {
        auto* doc = engine_.document();
        std::string current = (doc && doc->documentElement()) ? doc->documentElement()->innerHTML() : "";
        if (lastDump_.empty()) {
            std::cout << "(no previous dump to diff against — showing full HTML)\n";
            std::cout << current << "\n";
        } else if (lastDump_ == current) {
            std::cout << "(no changes)\n";
        } else {
            auto splitLines = [](const std::string& s) -> std::vector<std::string> {
                std::vector<std::string> lines;
                std::istringstream iss(s);
                std::string l;
                while (std::getline(iss, l)) lines.push_back(l);
                return lines;
            };
            auto oldLines = splitLines(lastDump_);
            auto newLines = splitLines(current);
            size_t maxLines = std::max(oldLines.size(), newLines.size());
            for (size_t i = 0; i < maxLines; i++) {
                std::string oldL = (i < oldLines.size()) ? oldLines[i] : "";
                std::string newL = (i < newLines.size()) ? newLines[i] : "";
                if (oldL != newL) {
                    if (!oldL.empty()) std::cout << "- " << oldL << "\n";
                    if (!newL.empty()) std::cout << "+ " << newL << "\n";
                }
            }
        }
        lastDump_ = current;
        return true;
    }

    if (cmd.substr(0, 6) == "click ") {
        std::string selector = cmd.substr(6);
        auto* doc = engine_.document();
        std::string before = (doc && doc->documentElement()) ? doc->documentElement()->innerHTML() : "";
        auto* el = engine_.querySelector(selector);
        if (!el) {
            std::cout << "[headless] click: element not found: " << selector << "\n";
        } else {
            engine_.dispatchClickOn(el);
            engine_.flush();
        }
        std::string after = (doc && doc->documentElement()) ? doc->documentElement()->innerHTML() : "";
        if (before != after) {
            std::cout << "[changed]\n";
        }
        return true;
    }

    if (cmd.substr(0, 5) == "eval ") {
        std::string code = cmd.substr(5);
        std::string result = engine_.eval(code);
        std::cout << result << "\n";
        return true;
    }

    if (cmd.substr(0, 5) == "wait ") {
        double ms = std::stod(cmd.substr(5));
        engine_.advanceTime(ms);
        return true;
    }

    if (cmd.substr(0, 11) == "screenshot ") {
        std::string path = cmd.substr(11);
        if (engine_.screenshot(path)) {
            std::cout << "[headless] saved screenshot to " << path << "\n";
        } else {
            std::cout << "[headless] screenshot failed\n";
        }
        return true;
    }

    if (cmd.substr(0, 5) == "rect ") {
        std::string selector = cmd.substr(5);
        auto* el = engine_.querySelector(selector);
        if (!el) {
            std::cout << "(not found: " << selector << ")\n";
            return true;
        }
        auto& box = el->layoutBox();
        std::cout << "x=" << box.contentRect.x << " y=" << box.contentRect.y
                  << " w=" << box.contentRect.width << " h=" << box.contentRect.height << "\n";
        return true;
    }

    if (cmd == "system" || cmd == "system toggle") {
        auto* overlay = engine_.systemOverlay();
        if (overlay) {
            overlay->toggle();
            std::cout << "[headless] system overlay "
                      << (overlay->isVisible() ? "visible" : "hidden") << "\n";
        }
        return true;
    }

    if (cmd.substr(0, 12) == "system perf ") {
        auto* overlay = engine_.systemOverlay();
        if (overlay) {
            std::istringstream iss(cmd.substr(12));
            double fps, ft, js, layout, raster, gpu, draw;
            if (iss >> fps >> ft >> js >> layout >> raster >> gpu >> draw) {
                overlay->updatePerf(fps, ft, js, layout, raster, gpu, draw,
                                    engine_.viewportWidth(), engine_.viewportHeight());
                overlay->tick(engine_.virtualTime());
            }
        }
        return true;
    }

    if (cmd == "help") {
        std::cout << "Commands:\n"
                  << "  dump              Dump full DOM as HTML\n"
                  << "  dump <selector>   Dump a single element's outer HTML\n"
                  << "  diff              Show changes since last dump\n"
                  << "  click <selector>  Simulate a click (e.g. click #btn)\n"
                  << "  eval <js>         Evaluate JavaScript, print result\n"
                  << "  wait <ms>         Advance virtual time by N ms\n"
                  << "  screenshot <path> Render to BMP file\n"
                  << "  rect <selector>   Print element's layout box\n"
                  << "  system            Toggle system overlay\n"
                  << "  quit              Exit\n"
                  << "  # comment         Ignored\n";
        return true;
    }

    std::cout << "[headless] unknown command: " << cmd << "\n";
    return true;
}

void HeadlessController::runInteractive() {
    std::cout << "bro headless> " << std::flush;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!processCommand(line)) break;
        std::cout << "bro headless> " << std::flush;
    }
}

void HeadlessController::runScript(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        std::cerr << "Cannot open script: " << path << "\n";
        return;
    }
    std::string line;
    while (std::getline(ifs, line)) {
        std::cout << "> " << line << "\n";
        if (!processCommand(line)) break;
    }
}

} // namespace bro::headless
