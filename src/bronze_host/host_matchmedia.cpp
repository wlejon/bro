#include "bronze_host/host_matchmedia.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_internal.h"
#include "dom/document.h"
#include "engine/engine.h"
#include "css/parser.h"
#include "util/log.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace {

struct MqlListener {
    // Identity for the abort callback, which outlives any address the
    // listener function had when it was registered (the collector moves it).
    uint64_t key = 0;
    ev::Persistent fn;
    bool once = false;
    bool capture = false;
    bool hasSignal = false;
    ev::Persistent signal;
    ev::Persistent abortCb;
};

struct MqlState {
    uint64_t id = 0;
    dom::Document* doc = nullptr;
    std::string media;
    bool lastMatches = false;
    std::vector<MqlListener> listeners;
    ev::Persistent onchange;
    ev::Persistent mqlObj;
};

static uint64_t s_nextMqlId = 1;
static uint64_t s_nextListenerKey = 1;
static std::unordered_map<uint64_t, std::shared_ptr<MqlState>> s_mqlStates;

} // namespace

void removeHostMediaQueriesForDocument(dom::Document* doc) {
    if (!doc) return;
    for (auto it = s_mqlStates.begin(); it != s_mqlStates.end();) {
        if (it->second->doc == doc) {
            for (auto& lit : it->second->listeners) {
                if (lit.hasSignal && !ev::isUndefined(lit.signal.get())) {
                    removeHostListener(lit.signal, "abort", lit.abortCb.get());
                }
            }
            it = s_mqlStates.erase(it);
        } else {
            ++it;
        }
    }
}

void clearHostMediaQueries() {
    for (auto& [id, st] : s_mqlStates) {
        if (st) {
            for (auto& lit : st->listeners) {
                if (lit.hasSignal && !ev::isUndefined(lit.signal.get())) {
                    removeHostListener(lit.signal, "abort", lit.abortCb.get());
                }
            }
        }
    }
    s_mqlStates.clear();
}

Value makeHostMatchMediaObject(const std::string& rawQuery) {
    std::string query = rawQuery;
    size_t start = query.find_first_not_of(" \t\r\n");
    size_t end = query.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) {
        query = "";
    } else {
        query = query.substr(start, end - start + 1);
    }
    std::string mediaStr = query.empty() ? "all" : query;

    dom::Document* doc = currentHostDocument();
    if (!doc && hostEngine()) doc = hostEngine()->document();

    bool matches = false;
    if (mediaStr == "all") {
        matches = true;
    } else if (doc) {
        auto mctx = doc->mediaContext();
        matches = htmlayout::css::evaluateMediaQuery(mediaStr, mctx);
    } else if (auto* eng = hostEngine()) {
        htmlayout::css::MediaContext mctx;
        mctx.viewportWidth = static_cast<float>(eng->contentWidth());
        mctx.viewportHeight = static_cast<float>(eng->contentHeight());
        matches = htmlayout::css::evaluateMediaQuery(mediaStr, mctx);
    }

    uint64_t id = s_nextMqlId++;
    auto state = std::make_shared<MqlState>();
    state->id = id;
    state->doc = doc;
    state->media = mediaStr;
    state->lastMatches = matches;
    s_mqlStates[id] = state;

    ObjectBuilder m;
    m.accessor("matches", [id](Value, std::span<const Value>) -> Value {
        auto it = s_mqlStates.find(id);
        if (it != s_mqlStates.end() && it->second->doc) {
            auto mctx = it->second->doc->mediaContext();
            bool live = htmlayout::css::evaluateMediaQuery(it->second->media, mctx);
            return ev::fromBool(live);
        }
        return ev::fromBool(false);
    }, nullptr);

    m.set("media", ev::fromUtf8(mediaStr));

    m.accessor("onchange",
        [id](Value, std::span<const Value>) -> Value {
            auto it = s_mqlStates.find(id);
            if (it != s_mqlStates.end() && !ev::isUndefined(it->second->onchange.get())) {
                return it->second->onchange.get();
            }
            return ev::null();
        },
        [id](Value, std::span<const Value> a) -> Value {
            auto it = s_mqlStates.find(id);
            if (it != s_mqlStates.end()) {
                if (!a.empty() && ev::isFunction(a[0])) {
                    it->second->onchange.set(a[0]);
                } else {
                    it->second->onchange.set(ev::null());
                }
            }
            return ev::undefined();
        });

    m.def("addEventListener", 3, [id](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2 || !ev::isFunction(a[1])) return ev::undefined();
        std::string type = ev::toUtf8(a[0]);
        if (type != "change") return ev::undefined();

        auto it = s_mqlStates.find(id);
        if (it == s_mqlStates.end()) return ev::undefined();

        // The option reads may run getters (and allocate), so the listener
        // and signal are rooted rather than held as raw copies.
        ev::Persistent fn(a[1]);
        bool capture = false;
        bool once = false;
        bool hasSignal = false;
        ev::Persistent signalV(ev::undefined());

        if (ev::isObject(argAt(a, 2))) {
            ev::Persistent optV(argAt(a, 2));
            capture = ev::toBool(ev::getProperty(optV.get(), "capture"));
            once = ev::toBool(ev::getProperty(optV.get(), "once"));
            Value sigV = ev::getProperty(optV.get(), "signal");
            if (ev::isObject(sigV)) {
                hasSignal = true;
                signalV.set(sigV);
            }
        } else if (!ev::isUndefined(argAt(a, 2))) {
            capture = ev::toBool(argAt(a, 2));
        }

        if (hasSignal) {
            Value ab = ev::getProperty(signalV.get(), "aborted");
            if (ev::toBool(ab)) return ev::undefined();
        }

        for (const auto& existing : it->second->listeners) {
            if (ev::toBits(existing.fn.get()) == ev::toBits(fn.get()) && existing.capture == capture) {
                return ev::undefined();
            }
        }

        MqlListener lit;
        lit.key = s_nextListenerKey++;
        lit.fn.set(fn.get());
        lit.once = once;
        lit.capture = capture;
        lit.hasSignal = hasSignal;

        if (hasSignal) {
            lit.signal.set(signalV.get());
            ev::Persistent sigP(signalV.get());
            const uint64_t key = lit.key;
            ev::Persistent abortCb(ev::makeFunction([id, key](Value, std::span<const Value>) {
                auto sit = s_mqlStates.find(id);
                if (sit != s_mqlStates.end()) {
                    auto& list = sit->second->listeners;
                    for (auto litIt = list.begin(); litIt != list.end(); ++litIt) {
                        if (litIt->key == key) {
                            if (litIt->hasSignal && !ev::isUndefined(litIt->signal.get())) {
                                removeHostListener(litIt->signal, "abort", litIt->abortCb.get());
                            }
                            list.erase(litIt);
                            break;
                        }
                    }
                }
                return ev::undefined();
            }, 0));
            lit.abortCb.set(abortCb.get());
            addHostListener(sigP, "abort", abortCb.get());
        }

        it->second->listeners.push_back(std::move(lit));
        return ev::undefined();
    });

    m.def("removeEventListener", 3, [id](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::undefined();
        std::string type = ev::toUtf8(a[0]);
        if (type != "change") return ev::undefined();

        auto it = s_mqlStates.find(id);
        if (it == s_mqlStates.end()) return ev::undefined();

        bool capture = false;
        if (ev::isObject(argAt(a, 2))) {
            capture = ev::toBool(ev::getProperty(argAt(a, 2), "capture"));
        } else if (!ev::isUndefined(argAt(a, 2))) {
            capture = ev::toBool(argAt(a, 2));
        }

        // Read after the option getter: a[1] is the slot the collector
        // updates, and nothing allocates between this read and the compare.
        uint64_t fnBits = ev::toBits(a[1]);
        auto& list = it->second->listeners;
        for (auto litIt = list.begin(); litIt != list.end(); ++litIt) {
            if (ev::toBits(litIt->fn.get()) == fnBits && litIt->capture == capture) {
                if (litIt->hasSignal && !ev::isUndefined(litIt->signal.get())) {
                    removeHostListener(litIt->signal, "abort", litIt->abortCb.get());
                }
                list.erase(litIt);
                break;
            }
        }
        return ev::undefined();
    });

    m.def("addListener", 1, [id](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isFunction(a[0])) return ev::undefined();
        auto it = s_mqlStates.find(id);
        if (it == s_mqlStates.end()) return ev::undefined();

        Value fn = a[0];
        uint64_t fnBits = ev::toBits(fn);
        for (const auto& existing : it->second->listeners) {
            if (ev::toBits(existing.fn.get()) == fnBits && !existing.capture) {
                return ev::undefined();
            }
        }

        MqlListener lit;
        lit.fn.set(fn);
        lit.once = false;
        lit.capture = false;
        lit.hasSignal = false;
        it->second->listeners.push_back(std::move(lit));
        return ev::undefined();
    });

    m.def("removeListener", 1, [id](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::undefined();
        auto it = s_mqlStates.find(id);
        if (it == s_mqlStates.end()) return ev::undefined();

        Value fn = a[0];
        uint64_t fnBits = ev::toBits(fn);
        auto& list = it->second->listeners;
        for (auto litIt = list.begin(); litIt != list.end(); ++litIt) {
            if (ev::toBits(litIt->fn.get()) == fnBits && !litIt->capture) {
                if (litIt->hasSignal && !ev::isUndefined(litIt->signal.get())) {
                    removeHostListener(litIt->signal, "abort", litIt->abortCb.get());
                }
                list.erase(litIt);
                break;
            }
        }
        return ev::undefined();
    });

    Value mqlVal = m.get();
    state->mqlObj.set(mqlVal);
    return mqlVal;
}

void deliverHostMediaQueryChanges() {
    if (s_mqlStates.empty()) return;

    for (auto it = s_mqlStates.begin(); it != s_mqlStates.end();) {
        if (!it->second || !it->second->doc || !dom::Document::isLiveDocument(it->second->doc)) {
            if (it->second) {
                for (auto& lit : it->second->listeners) {
                    if (lit.hasSignal && !ev::isUndefined(lit.signal.get())) {
                        removeHostListener(lit.signal, "abort", lit.abortCb.get());
                    }
                }
            }
            it = s_mqlStates.erase(it);
        } else {
            ++it;
        }
    }

    std::vector<std::shared_ptr<MqlState>> states;
    states.reserve(s_mqlStates.size());
    for (auto& [id, st] : s_mqlStates) {
        if (st && st->doc && dom::Document::isLiveDocument(st->doc)) states.push_back(st);
    }

    for (auto& st : states) {
        if (!st->doc || !dom::Document::isLiveDocument(st->doc)) continue;
        if (st->doc->mediaRestylePending()) {
            st->doc->resolveStyles();
        }

        auto mctx = st->doc->mediaContext();
        bool freshMatches = htmlayout::css::evaluateMediaQuery(st->media, mctx);
        if (freshMatches != st->lastMatches) {
            st->lastMatches = freshMatches;

            ObjectBuilder evt;
            evt.set("type", ev::fromUtf8("change"));
            evt.set("matches", ev::fromBool(freshMatches));
            evt.set("media", ev::fromUtf8(st->media));
            evt.set("target", st->mqlObj.get());
            // Dispatched at the list itself: it is also the current target,
            // at-target phase, and the event neither bubbles nor cancels.
            evt.set("currentTarget", st->mqlObj.get());
            evt.set("srcElement", st->mqlObj.get());
            evt.set("eventPhase", ev::fromDouble(2));
            evt.set("bubbles", ev::fromBool(false));
            evt.set("cancelable", ev::fromBool(false));
            evt.set("defaultPrevented", ev::fromBool(false));
            evt.set("isTrusted", ev::fromBool(true));
            // Everything held across the listener calls is rooted: the calls,
            // the document wrapper and the property writes all allocate.
            ev::Persistent evRoot(evt.get());

            dom::Document* prevDoc = currentHostDocument();
            ev::GlobalValue docG = ev::globalValue("document");
            ev::GlobalValue gtG = ev::globalValue("globalThis");
            const bool haveGlobal = gtG.found && ev::isObject(gtG.value);
            ev::Persistent global(haveGlobal ? gtG.value : ev::undefined());
            ev::Persistent prevDocVal(docG.found ? docG.value : ev::null());
            if (st->doc) {
                setCurrentHostDocument(st->doc);
                ev::Persistent subDocVal(hostDocumentValue(st->doc));
                ev::registerGlobal("document", subDocVal.get());
                if (haveGlobal) {
                    ev::setProperty(global.get(), "document", subDocVal.get());
                }
            }

            std::vector<MqlListener> toInvoke;
            toInvoke.reserve(st->listeners.size());
            for (auto it = st->listeners.begin(); it != st->listeners.end();) {
                if (it->hasSignal && !ev::isUndefined(it->signal.get())) {
                    Value ab = ev::getProperty(it->signal.get(), "aborted");
                    if (ev::toBool(ab)) {
                        removeHostListener(it->signal, "abort", it->abortCb.get());
                        it = st->listeners.erase(it);
                        continue;
                    }
                }
                toInvoke.push_back(*it);
                if (it->once) {
                    if (it->hasSignal && !ev::isUndefined(it->signal.get())) {
                        removeHostListener(it->signal, "abort", it->abortCb.get());
                    }
                    it = st->listeners.erase(it);
                } else {
                    ++it;
                }
            }

            for (auto& lit : toInvoke) {
                if (ev::isFunction(lit.fn.get())) {
                    Value evVal = evRoot.get();
                    ev::CallResult r = ev::call(lit.fn.get(), st->mqlObj.get(),
                                                std::span<const Value>(&evVal, 1));
                    if (r.thrown) reportBronzeError("matchMedia listener", r.value);
                }
            }
            if (!ev::isUndefined(st->onchange.get()) && !ev::isNull(st->onchange.get()) &&
                ev::isFunction(st->onchange.get())) {
                Value evVal = evRoot.get();
                ev::CallResult r = ev::call(st->onchange.get(), st->mqlObj.get(),
                                            std::span<const Value>(&evVal, 1));
                if (r.thrown) reportBronzeError("matchMedia onchange", r.value);
            }

            if (st->doc && dom::Document::isLiveDocument(st->doc)) {
                if (!ev::isNull(prevDocVal.get())) {
                    ev::registerGlobal("document", prevDocVal.get());
                    if (haveGlobal) {
                        ev::setProperty(global.get(), "document", prevDocVal.get());
                    }
                }
                setCurrentHostDocument(prevDoc);
                st->doc->markDirty();
            }
        }
    }
}

} // namespace bro::bronze_host
