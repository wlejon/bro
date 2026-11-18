// Script elements a page inserts (HTML §4.12.1.1, "prepare the script
// element" when a script becomes connected): an inline classic script runs at
// once, inside the appendChild / insertBefore / append that connected it, as
// jQuery's globalEval and every inline loader expect; one with a src is
// fetched and run on a later task, then fires `load`, or `error` when the
// fetch failed (missing, unreachable, refused by the app's permissions).
//
// A script runs once: dom::Element::scriptStarted is HTML's "already started",
// set by the parser for the page's own scripts and for those innerHTML makes
// (which never run), and cleared by createContextualFragment, whose scripts
// do. Module scripts and data blocks are left alone.

#include "bronze_host/host_inserted_scripts.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/eval_jit.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_events.h"
#include "bronze_host/host_url_reflect.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/node_handle.h"
#include "dom/shadow_root.h"
#include "engine/app_loader.h"
#include "engine/engine.h"

#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// In its document's tree, through shadow roots to their hosts.
bool isConnected(dom::Node* node) {
    dom::Document* doc = node ? node->document() : nullptr;
    dom::Node* root = doc ? doc->documentElement() : nullptr;
    for (dom::Node* n = node; n && root;) {
        if (n == root) return true;
        dom::Node* parent = n->parentNode();
        if (!parent) {
            auto* sr = dynamic_cast<dom::ShadowRoot*>(n);
            if (!sr) return false;
            parent = sr->host();
        }
        n = parent;
    }
    return false;
}

std::string trimmedLowerType(const dom::Element* el) {
    std::string type = el->getAttribute("type");
    if (type.empty() && el->hasAttribute("language")) type = "text/" + el->getAttribute("language");
    for (char& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t b = type.find_first_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : type.substr(b, type.find_last_not_of(" \t\r\n") - b + 1);
}

void fireScriptEvent(dom::Element* el, const char* type) {
    dom::Event evt(type, false, false);
    evt.setIsTrusted(true);
    if (auto* eng = hostEngine()) eng->dispatchElementEvent(el, evt);
}

void runScriptText(const std::string& code, const std::string& name) {
    auto* eng = hostEngine();
    if (!eng) return;
    ev::CallResult r = evalScriptJitResult(*eng, code, name, nullptr, false, ScriptGoal::ClassicScript);
    if (r.thrown) {
        ev::Persistent thrown(r.value);
        if (!hostDispatchUncaughtError(thrown.get())) reportBronzeError(name.c_str(), thrown.get());
    }
}

// The file a script's src names: a bro://app/ URL's file, an http(s) URL as
// is (AppLoader fetches it through the app's `net`), else none.
std::string scriptSourcePath(const std::string& url) {
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) return url;
    auto* eng = hostEngine();
    return eng ? eng->pathForAppUrl(url) : std::string();
}

void prepareScript(dom::Element* el) {
    if (el->scriptStarted() || el->ns() != dom::Element::Namespace::HTML) return;
    if (!engine::AppLoader::isClassicScriptType(trimmedLowerType(el))) return;
    const bool external = el->hasAttribute("src");
    std::string text;
    if (!external) {
        text = el->textContent();
        if (text.empty()) return;  // not started: text inserted later runs it
    }
    el->setScriptStarted(true);
    dom::Document* doc = el->document();
    if (!external) {
        runScriptText(text, documentUrl(doc));
        return;
    }
    const std::string url = reflectedUrl(el, "src");
    dom::ElementHandle handle(doc, el);
    postHostTask([handle, url]() mutable {
        dom::Element* target = handle.get();
        if (!target) return;
        const std::string path = scriptSourcePath(url);
        std::optional<std::string> body;
        if (!path.empty()) body = engine::AppLoader::readFile(path);
        if (!body) {
            fireScriptEvent(target, "error");
            return;
        }
        runScriptText(*body, url);
        if (dom::Element* again = handle.get()) fireScriptEvent(again, "load");
    });
}

}  // namespace

void runInsertedScripts(dom::Element* root) {
    if (!root || !isConnected(root)) return;
    std::vector<dom::ElementHandle> scripts;
    dom::Document* doc = root->document();
    if (root->tagName() == "script" || root->tagName() == "SCRIPT") {
        if (!root->scriptStarted()) scripts.emplace_back(doc, root);
    } else if (!root->childNodes().empty()) {
        for (dom::Element* el : root->querySelectorAll("script")) {
            if (!el->scriptStarted()) scripts.emplace_back(doc, el);
        }
    }
    // Each in tree order; one may remove or insert the others.
    for (auto& h : scripts) {
        dom::Element* el = h.get();
        if (el && isConnected(el)) prepareScript(el);
    }
}

}  // namespace bro::bronze_host
