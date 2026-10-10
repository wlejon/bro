#include "layout/image_loading.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/node_handle.h"
#include "render/image_source.h"
#include "render/image_store.h"

#include <memory>
#include <utility>
#include <vector>

namespace bro::layout {

namespace {

struct PendingLoad {
    dom::ElementHandle el;
    std::string src;  // the src attribute this load is for
    std::shared_ptr<render::ImageRequest> req;
    std::vector<std::function<void(bool)>> waiters;
};

std::vector<PendingLoad>& pending() {
    static std::vector<PendingLoad> list;
    return list;
}

uint64_t g_lastSettled = 0;
bool g_paintWaits = false;

void queueEvent(dom::Element* el, bool ok) {
    if (auto* doc = el->document()) doc->queueElementEvent(el, ok ? "load" : "error");
}

// The load in flight for `el`, taken out of the list (its waiters with it).
bool takePending(dom::Element* el, PendingLoad& out) {
    auto& list = pending();
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->el.get() == el) {
            out = std::move(*it);
            list.erase(it);
            return true;
        }
    }
    return false;
}

void broken(dom::Element* el, const std::string& src, std::shared_ptr<render::ImageRequest> req) {
    el->setImageNaturalSize(src, 0, 0);
    el->setImageLoadState(std::move(req), /*complete=*/true, /*ok=*/false, 1);
    queueEvent(el, false);
}

void available(dom::Element* el, const std::string& src, std::shared_ptr<render::ImageRequest> req) {
    auto img = req->image();
    el->setImageNaturalSize(src, img->width, img->height);
    el->setImageLoadState(std::move(req), /*complete=*/true, /*ok=*/true, img->orientation);
    queueEvent(el, true);
}

}  // namespace

void loadImageElement(dom::Element* el, const std::string& src) {
    if (!el) return;

    // A new src supersedes the load in flight: its decode() promises settle
    // as failed (the image they waited for is not the one shown).
    {
        PendingLoad old;
        if (takePending(el, old))
            for (auto& w : old.waiters) w(false);
    }

    if (src.empty()) {
        el->setImageNaturalSize("", 0, 0);
        el->setImageLoadState(nullptr, /*complete=*/true, /*ok=*/false, 1);
        return;
    }

    dom::Document* doc = el->document();
    const render::ImageSource source =
        render::resolveImageSource(src, doc ? doc->basePath() : std::string(), /*oriented=*/true);
    std::shared_ptr<render::ImageRequest> req = render::requestImage(source);
    if (!req) {
        broken(el, src, nullptr);
        return;
    }
    if (req->ready()) {
        available(el, src, std::move(req));
        return;
    }
    if (req->settled()) {
        broken(el, src, std::move(req));
        return;
    }

    // Decoding: the box takes its size from the header now, where reading one
    // is cheap, and the pixels arrive with the pump.
    int w = 0, h = 0, orientation = 1;
    bool isSvg = false;
    if (render::probeImageSource(source, w, h, orientation, isSvg))
        el->setImageNaturalSize(src, w, h);
    else
        el->setImageNaturalSize(src, 0, 0);
    el->setImageLoadState(req, /*complete=*/false, /*ok=*/false, orientation);

    PendingLoad load;
    load.el.assign(doc, el);
    load.src = src;
    load.req = std::move(req);
    pending().push_back(std::move(load));
}

bool pumpImageLoads() {
    const uint64_t settled = render::ImageStore::instance().settledCount();
    if (settled == g_lastSettled) return false;
    g_lastSettled = settled;

    // Settle into a batch first: a waiter may start another load.
    std::vector<std::pair<dom::Element*, PendingLoad>> done;
    auto& list = pending();
    for (auto it = list.begin(); it != list.end();) {
        dom::Element* el = it->el.get();
        if (!el) {
            it = list.erase(it);
            continue;
        }
        if (!it->req->settled()) {
            ++it;
            continue;
        }
        done.emplace_back(el, std::move(*it));
        it = list.erase(it);
    }
    for (auto& [el, load] : done) {
        const bool ok = load.req->ready();
        if (ok)
            available(el, load.src, load.req);
        else
            broken(el, load.src, load.req);
        for (auto& w : load.waiters) w(ok);
    }
    // A CSS background or border-image waiting on a decode has no element to
    // tell: every document repaints.
    dom::Document::markAllPaintDirty();
    return true;
}

bool imageSettlesPending() {
    return render::ImageStore::instance().settledCount() != g_lastSettled;
}

void settleImageLoads(double timeoutMs) {
    render::ImageStore::instance().waitIdle(timeoutMs);
    pumpImageLoads();
}

void whenImageSettled(dom::Element* el, std::function<void(bool ok)> cb) {
    if (!el || !cb) return;
    for (auto& load : pending()) {
        if (load.el.get() == el) {
            load.waiters.push_back(std::move(cb));
            return;
        }
    }
    cb(el->imageComplete() && el->imageOk());
}

void setPaintWaitsForImages(bool wait) { g_paintWaits = wait; }
bool paintWaitsForImages() { return g_paintWaits; }

}  // namespace bro::layout
