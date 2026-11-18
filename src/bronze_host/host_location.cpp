// `location` and `history`: the document's URL (dom::Document::url, a
// `bro://app/` URL of the app folder's page) read and written through the
// web's two objects. Each realm has its own pair, and every member answers
// for that realm's document whichever realm calls it (compartmentDocument):
// `frame.contentWindow.location` is the frame's.
//
// location's URL members read the document's URL; writing them, assign() and
// replace() navigate: to a fragment of the same page in place (the URL
// changes, a history entry is added, hashchange fires), to another page of
// the app through the engine (Engine::navigateDocument, which loads it in the
// document's place), to an http(s) page in the system browser.
//
// history keeps the document's session history: its entries' URLs and states
// (pushState / replaceState, which change the URL without loading anything),
// and back / forward / go over them, which fire popstate (and hashchange when
// the fragment changes). An entry of another page is not kept: navigating
// away starts the next page's history afresh.

#include "bronze_host/host_builder.h"
#include "bronze_host/host_compartment.h"
#include "bronze_host/host_events.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_global_kinds.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"
#include "bronze_host/host_web_globals.h"
#include "bronze_host/host_window_open.h"

#include "dom/document.h"
#include "engine/engine.h"
#include "util/remote_asset.h"

#include <cctype>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

constexpr const char* kAppOrigin = "bro://app";

}  // namespace

std::string documentUrl(dom::Document* doc) {
    if (doc && !doc->url().empty()) return doc->url();
    return std::string(kAppOrigin) + "/";
}

namespace {

struct UrlParts {
    std::string protocol, host, pathname, search, hash;
};

UrlParts splitUrl(const std::string& url) {
    UrlParts u;
    size_t pos = 0;
    const size_t scheme = url.find("://");
    if (scheme != std::string::npos) {
        u.protocol = url.substr(0, scheme + 1);
        pos = scheme + 3;
        const size_t pathStart = url.find_first_of("/?#", pos);
        u.host = url.substr(pos, pathStart == std::string::npos ? std::string::npos : pathStart - pos);
        pos = pathStart == std::string::npos ? url.size() : pathStart;
    }
    const size_t hashAt = url.find('#', pos);
    const size_t searchAt = url.find('?', pos);
    const size_t pathEnd = std::min(hashAt, searchAt);
    u.pathname = url.substr(pos, pathEnd == std::string::npos ? std::string::npos : pathEnd - pos);
    if (u.pathname.empty()) u.pathname = "/";
    if (searchAt != std::string::npos && searchAt < hashAt) {
        u.search = url.substr(searchAt, hashAt == std::string::npos ? std::string::npos : hashAt - searchAt);
    }
    if (hashAt != std::string::npos) u.hash = url.substr(hashAt);
    // An empty query or fragment reads as "", as the URL standard's getters do.
    if (u.search == "?") u.search.clear();
    if (u.hash == "#") u.hash.clear();
    return u;
}

std::string withoutFragment(const std::string& url) {
    return url.substr(0, url.find('#'));
}

// The session history of a document (hostSlots "history").
struct HistoryEntry {
    std::string url;
    ev::Persistent state;
};
struct SessionHistory {
    std::vector<std::shared_ptr<HistoryEntry>> entries;
    size_t index = 0;
};

SessionHistory& historyOf(dom::Document* doc) {
    auto& slot = doc->hostSlots()["history"];
    if (!slot) {
        auto h = std::make_shared<SessionHistory>();
        auto first = std::make_shared<HistoryEntry>();
        first->url = documentUrl(doc);
        first->state.set(ev::null());
        h->entries.push_back(first);
        slot = h;
    }
    return *std::static_pointer_cast<SessionHistory>(slot);
}

// Fires `type` at the document's window with the caller's fields on the event
// object (PopStateEvent's state, HashChangeEvent's URLs).
void fireWindowEvent(dom::Document* doc, const char* type,
                     const std::vector<std::pair<const char*, Value>>& fields) {
    std::vector<ev::Persistent> values;
    for (const auto& f : fields) values.emplace_back(f.second);
    ev::Persistent typeV(ev::fromUtf8(type));
    ev::Persistent desc(ev::createObject());
    desc.set(ev::setProperty(desc.get(), "type", typeV.get()));
    for (size_t i = 0; i < fields.size(); ++i) {
        desc.set(ev::setProperty(desc.get(), fields[i].first, values[i].get()));
    }
    hostDispatchToWindowOf(doc, desc.get());
}

void fireHashChange(dom::Document* doc, const std::string& oldUrl, const std::string& newUrl) {
    ev::Persistent oldV(ev::fromUtf8(oldUrl));
    ev::Persistent newV(ev::fromUtf8(newUrl));
    fireWindowEvent(doc, "hashchange", {{"oldURL", oldV.get()}, {"newURL", newV.get()}});
}

// Adds (or, `replace`, replaces) the current entry with one for `url` and
// the state `state`, and makes `url` the document's.
void addHistoryEntry(dom::Document* doc, const std::string& url, Value state, bool replace) {
    ev::Persistent stateP(state);
    SessionHistory& h = historyOf(doc);
    auto entry = std::make_shared<HistoryEntry>();
    entry->url = url;
    entry->state.set(stateP.get());
    if (replace) {
        h.entries[h.index] = entry;
    } else {
        h.entries.resize(h.index + 1);
        h.entries.push_back(entry);
        h.index = h.entries.size() - 1;
    }
    doc->setUrl(url);
}

// Navigates `doc` to `href` (resolved against its URL).
Value navigate(dom::Document* doc, const std::string& href, bool replace) {
    if (!doc) return ev::undefined();
    const std::string current = documentUrl(doc);
    const std::string url = util::resolveUrl(current, href);
    // A fragment of this page: the URL changes in place.
    if (url.find('#') != std::string::npos && withoutFragment(url) == withoutFragment(current)) {
        if (url == current) return ev::undefined();
        addHistoryEntry(doc, url, ev::null(), replace);
        fireHashChange(doc, current, url);
        return ev::undefined();
    }
    if (engine::Engine* e = hostEngine()) e->navigateDocument(doc, url);
    return ev::undefined();
}

// The URL `href` with one part replaced: what the location setters navigate to.
std::string withPart(const std::string& href, const char* part, std::string value) {
    UrlParts u = splitUrl(href);
    const std::string p = part;
    if (p == "hash") {
        if (!value.empty() && value[0] == '#') value.erase(0, 1);
        u.hash = "#" + value;
    } else if (p == "search") {
        if (!value.empty() && value[0] == '?') value.erase(0, 1);
        u.search = value.empty() ? std::string() : "?" + value;
    } else if (p == "pathname") {
        u.pathname = !value.empty() && value[0] == '/' ? value : "/" + value;
    }
    return u.protocol + "//" + u.host + u.pathname + u.search + u.hash;
}

using PartOf = std::string (*)(const UrlParts&);

void urlAccessor(ObjectBuilder& b, uint64_t home, const char* name, PartOf part, const char* settable) {
    std::string setPart = settable ? settable : "";
    b.accessor(
        name,
        [home, part](Value, std::span<const Value>) -> Value {
            return ev::fromUtf8(part(splitUrl(documentUrl(compartmentDocument(home)))));
        },
        [home, setPart](Value, std::span<const Value> a) -> Value {
            if (setPart.empty()) return ev::undefined();
            dom::Document* doc = compartmentDocument(home);
            std::string value = ev::toUtf8(argAt(a, 0));
            if (setPart == "href") return navigate(doc, value, false);
            return navigate(doc, withPart(documentUrl(doc), setPart.c_str(), value), false);
        });
}

// location.reload(), history.go(0): the frame's document reloads its frame,
// the app's the app.
Value reloadDocument(dom::Document* doc) {
    engine::Engine* eng = hostEngine();
    if (!doc || !eng) return ev::undefined();
    if (!eng->reloadIframeForDocument(doc) && doc == eng->document()) eng->requestAppReload();
    return ev::undefined();
}

Value makeLocation(uint64_t home) {
    ObjectBuilder loc;
    urlAccessor(loc, home, "href", [](const UrlParts& u) { return u.protocol + "//" + u.host + u.pathname + u.search + u.hash; }, "href");
    urlAccessor(loc, home, "protocol", [](const UrlParts& u) { return u.protocol; }, nullptr);
    urlAccessor(loc, home, "host", [](const UrlParts& u) { return u.host; }, nullptr);
    urlAccessor(loc, home, "hostname", [](const UrlParts& u) { return u.host; }, nullptr);
    urlAccessor(loc, home, "port", [](const UrlParts&) { return std::string(); }, nullptr);
    urlAccessor(loc, home, "origin", [](const UrlParts& u) { return u.protocol + "//" + u.host; }, nullptr);
    urlAccessor(loc, home, "pathname", [](const UrlParts& u) { return u.pathname; }, "pathname");
    urlAccessor(loc, home, "search", [](const UrlParts& u) { return u.search; }, "search");
    urlAccessor(loc, home, "hash", [](const UrlParts& u) { return u.hash; }, "hash");
    loc.def("toString", 0, [home](Value, std::span<const Value>) -> Value {
        return ev::fromUtf8(documentUrl(compartmentDocument(home)));
    });
    loc.def("assign", 1, [home](Value, std::span<const Value> a) -> Value {
        return navigate(compartmentDocument(home), ev::toUtf8(argAt(a, 0)), false);
    });
    loc.def("replace", 1, [home](Value, std::span<const Value> a) -> Value {
        return navigate(compartmentDocument(home), ev::toUtf8(argAt(a, 0)), true);
    });
    loc.def("reload", 0, [home](Value, std::span<const Value>) -> Value {
        return reloadDocument(compartmentDocument(home));
    });
    return loc.get();
}

// pushState / replaceState: the document's URL becomes `url` (resolved; it
// must stay in the app's origin) with no load.
Value changeState(dom::Document* doc, std::span<const Value> a, bool replace) {
    if (!doc) return ev::undefined();
    ev::Persistent state(argAt(a, 0));
    std::string url = documentUrl(doc);
    Value urlV = argAt(a, 2);
    if (!ev::isUndefined(urlV) && !ev::isNull(urlV)) {
        url = util::resolveUrl(url, ev::toUtf8(urlV));
        if (url.compare(0, std::string(kAppOrigin).size() + 1, std::string(kAppOrigin) + "/") != 0) {
            return ev::throwValue(hostMakeDomError(
                "SecurityError", std::string(replace ? "replaceState" : "pushState") + ": '" + url +
                                     "' is not of the page's origin " + kAppOrigin));
        }
    }
    addHistoryEntry(doc, url, state.get(), replace);
    return ev::undefined();
}

// back / forward / go: to the entry `delta` away, if there is one.
Value traverse(dom::Document* doc, int delta) {
    if (!doc) return ev::undefined();
    if (delta == 0) return reloadDocument(doc);
    SessionHistory& h = historyOf(doc);
    const long target = static_cast<long>(h.index) + delta;
    if (target < 0 || target >= static_cast<long>(h.entries.size())) return ev::undefined();
    const std::string oldUrl = documentUrl(doc);
    h.index = static_cast<size_t>(target);
    auto entry = h.entries[h.index];  // kept: a listener may push entries
    doc->setUrl(entry->url);
    fireWindowEvent(doc, "popstate", {{"state", entry->state.get()}});
    if (entry->url != oldUrl && withoutFragment(entry->url) == withoutFragment(oldUrl)) {
        fireHashChange(doc, oldUrl, entry->url);
    }
    return ev::undefined();
}

Value makeHistory(uint64_t home) {
    ObjectBuilder hist;
    hist.accessor("length", [home](Value, std::span<const Value>) -> Value {
        dom::Document* doc = compartmentDocument(home);
        return ev::fromDouble(doc ? static_cast<double>(historyOf(doc).entries.size()) : 1.0);
    }, nullptr);
    hist.accessor("state", [home](Value, std::span<const Value>) -> Value {
        dom::Document* doc = compartmentDocument(home);
        if (!doc) return ev::null();
        SessionHistory& h = historyOf(doc);
        return h.entries[h.index]->state.get();
    }, nullptr);
    hist.set("scrollRestoration", ev::fromUtf8("auto"));
    hist.def("back", 0, [home](Value, std::span<const Value>) { return traverse(compartmentDocument(home), -1); });
    hist.def("forward", 0, [home](Value, std::span<const Value>) { return traverse(compartmentDocument(home), 1); });
    hist.def("go", 1, [home](Value, std::span<const Value> a) {
        Value d = argAt(a, 0);
        return traverse(compartmentDocument(home), ev::isUndefined(d) ? 0 : static_cast<int>(ev::toDouble(d)));
    });
    hist.def("pushState", 3, [home](Value, std::span<const Value> a) {
        return changeState(compartmentDocument(home), a, false);
    });
    hist.def("replaceState", 3, [home](Value, std::span<const Value> a) {
        return changeState(compartmentDocument(home), a, true);
    });
    return hist.get();
}

}  // namespace

std::string documentBaseUrl(dom::Document* doc) {
    const std::string url = documentUrl(doc);
    if (!doc) return url;
    for (dom::Element* base : doc->querySelectorAll("base")) {
        if (base->hasAttribute("href")) return util::resolveUrl(url, base->getAttribute("href"));
    }
    return url;
}

void hostFollowHyperlink(dom::Element* anchor) {
    if (!anchor) return;
    dom::Document* doc = anchor->document();
    std::string href = anchor->getAttribute("href");
    // A javascript: URL is script, and bro runs no script out of a URL: a
    // `href="javascript:void(0)"` link does nothing, which is what it is
    // written to do.
    std::string scheme = href.substr(0, href.find(':') == std::string::npos ? 0 : href.find(':'));
    for (char& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (scheme == "javascript") return;
    const std::string url = util::resolveUrl(documentBaseUrl(doc), href);
    std::string target = anchor->getAttribute("target");
    for (char& c : target) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // A new browsing context (_blank, or a name): window.open's.
    if (!target.empty() && target != "_self" && target != "_parent" && target != "_top") {
        ev::Persistent urlV(ev::fromUtf8(url));
        ev::Persistent targetV(ev::fromUtf8(target));
        const Value args[2] = {urlV.get(), targetV.get()};
        handleWindowOpen(std::span<const Value>(args, 2));
        return;
    }
    navigate(doc, url, false);
}

void installLocationAndHistory(ObjectBuilder& window) {
    const uint64_t home = currentCompartmentId();
    // location is [LegacyUnforgeable, PutForwards=href]: an accessor whose
    // write navigates as `location.href = v` does.
    auto loc = std::make_shared<ev::Persistent>(makeLocation(home));
    registerHostGlobalAccessor(
        "location", [loc](Value, std::span<const Value>) { return loc->get(); },
        [home](Value, std::span<const Value> a) {
            dom::Document* doc = compartmentDocument(home);
            return navigate(doc, ev::toUtf8(argAt(a, 0)), false);
        },
        /*configurable=*/false);
    registerHostGlobal("history", makeHistory(home), ev::GlobalKind::Readonly);
    // window.origin, [Replaceable]: the page's origin, serialized.
    defineReplaceableAttribute(window, "origin", [home](Value, std::span<const Value>) {
        const UrlParts u = splitUrl(documentUrl(compartmentDocument(home)));
        return ev::fromUtf8(u.protocol + "//" + u.host);
    });
}

}  // namespace bro::bronze_host
