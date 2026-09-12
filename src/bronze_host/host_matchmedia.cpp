#include "bronze_host/host_matchmedia.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/host_globals_internal.h"
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

struct MqlState {
    uint64_t id = 0;
    dom::Document* doc = nullptr;
    std::string media;
    bool lastMatches = false;
    std::vector<ev::Persistent> listeners;
    ev::Persistent onchange;
    ev::Persistent mqlObj;
};

static uint64_t s_nextMqlId = 1;
static std::unordered_map<uint64_t, std::shared_ptr<MqlState>> s_mqlStates;

} // namespace

void removeHostMediaQueriesForDocument(dom::Document* doc) {
    if (!doc) return;
    for (auto it = s_mqlStates.begin(); it != s_mqlStates.end();) {
        if (it->second->doc == doc) {
            it = s_mqlStates.erase(it);
        } else {
            ++it;
        }
    }
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

    m.def("addEventListener", 2, [id](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2 || !ev::isFunction(a[1])) return ev::undefined();
        std::string type = ev::toUtf8(a[0]);
        if (type == "change") {
            auto it = s_mqlStates.find(id);
            if (it != s_mqlStates.end()) {
                it->second->listeners.emplace_back(a[1]);
            }
        }
        return ev::undefined();
    });

    m.def("removeEventListener", 2, [id](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::undefined();
        std::string type = ev::toUtf8(a[0]);
        if (type == "change") {
            auto it = s_mqlStates.find(id);
            if (it != s_mqlStates.end()) {
                auto& list = it->second->listeners;
                for (auto lit = list.begin(); lit != list.end(); ++lit) {
                    if (lit->get() == a[1]) {
                        list.erase(lit);
                        break;
                    }
                }
            }
        }
        return ev::undefined();
    });

    m.def("addListener", 1, [id](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !ev::isFunction(a[0])) return ev::undefined();
        auto it = s_mqlStates.find(id);
        if (it != s_mqlStates.end()) {
            it->second->listeners.emplace_back(a[0]);
        }
        return ev::undefined();
    });

    m.def("removeListener", 1, [id](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::undefined();
        auto it = s_mqlStates.find(id);
        if (it != s_mqlStates.end()) {
            auto& list = it->second->listeners;
            for (auto lit = list.begin(); lit != list.end(); ++lit) {
                if (lit->get() == a[0]) {
                    list.erase(lit);
                    break;
                }
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

    std::vector<std::shared_ptr<MqlState>> states;
    states.reserve(s_mqlStates.size());
    for (auto& [id, st] : s_mqlStates) {
        if (st && st->doc) states.push_back(st);
    }

    for (auto& st : states) {
        if (!st->doc) continue;
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
            Value evVal = evt.get();

            dom::Document* prevDoc = currentHostDocument();
            ev::GlobalValue docG = ev::globalValue("document");
            ev::GlobalValue gt = ev::globalValue("globalThis");
            Value prevDocVal = docG.found ? docG.value : ev::null();
            if (st->doc) {
                setCurrentHostDocument(st->doc);
                Value subDocVal = hostDocumentValue(st->doc);
                ev::registerGlobal("document", subDocVal);
                if (gt.found && ev::isObject(gt.value)) {
                    ev::setProperty(gt.value, "document", subDocVal);
                }
            }

            auto listenersCopy = st->listeners;
            for (auto& fn : listenersCopy) {
                if (ev::isFunction(fn.get())) {
                    ev::CallResult r = ev::call(fn.get(), st->mqlObj.get(),
                                                std::span<const Value>(&evVal, 1));
                    if (r.thrown) reportBronzeError("matchMedia listener", r.value);
                }
            }
            if (!ev::isUndefined(st->onchange.get()) && !ev::isNull(st->onchange.get()) &&
                ev::isFunction(st->onchange.get())) {
                ev::CallResult r = ev::call(st->onchange.get(), st->mqlObj.get(),
                                            std::span<const Value>(&evVal, 1));
                if (r.thrown) reportBronzeError("matchMedia onchange", r.value);
            }

            if (st->doc) {
                if (!ev::isNull(prevDocVal)) {
                    ev::registerGlobal("document", prevDocVal);
                    if (gt.found && ev::isObject(gt.value)) {
                        ev::setProperty(gt.value, "document", prevDocVal);
                    }
                }
                setCurrentHostDocument(prevDoc);
            }

            st->doc->markDirty();
        }
    }
}

} // namespace bro::bronze_host
