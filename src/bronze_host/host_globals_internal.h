#pragma once

#include "bronze_host/host_internal.h"
#include <include/core/SkImage.h>
#include <span>
#include <string>
#include <vector>

namespace bro::engine { class Engine; }

namespace bro::bronze_host {

// ---------------------------------------------------------------------------
// ImageBitmap & ImageData
// ---------------------------------------------------------------------------

struct HostImageBitmap {
    uint32_t tag = 0x4849424D; // 'HIBM'
    sk_sp<SkImage> image;
    std::vector<uint8_t> pixels; // RGBA8
    int width = 0;
    int height = 0;
    bool closed = false;
};

const HostImageBitmap* hostImageBitmapOf(Value v);
HostImageBitmap* hostImageBitmapOfMut(Value v);
Value wrapHostImageBitmap(sk_sp<SkImage> img);
Value wrapHostImageBitmap(const uint8_t* rgba, int w, int h);
void installImageBitmapGlobals();

// ImageData helpers
Value makeImageDataValue(int width, int height, Value dataArr);
Value makeImageDataValue(int width, int height, const uint8_t* pixels);

// ---------------------------------------------------------------------------
// FastNoise
// ---------------------------------------------------------------------------

void installNoiseGlobals();

// ---------------------------------------------------------------------------
// customElements
// ---------------------------------------------------------------------------

void installCustomElementsGlobals();
// Connect/disconnect walk the inserted subtree: a registered element is
// upgraded on the way in (parsed markup carries no constructor call) and
// every upgraded element in the subtree gets its lifecycle callback.
void onCustomElementConnected(dom::Element* el);
void onCustomElementDisconnected(dom::Element* el);
// Upgrade the registered elements under `root` (root excluded) — the hook
// for HTML that arrives through a parser (innerHTML) rather than an insert.
// Elements already in the document also get connectedCallback.
void upgradeCustomElementsInSubtree(dom::Node* root);
void onCustomElementAttributeChanged(dom::Element* el, const std::string& name,
                                     const char* oldValue, const char* newValue);
Value constructCustomElement(dom::Element* el, const std::string& tagName);
Value constructCustomElementBase();

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

void installWorkerGlobals(engine::Engine& engine);
void drainWorkerMessages();
void terminateAllWorkers();

// ---------------------------------------------------------------------------
// Combined extensions installer
// ---------------------------------------------------------------------------

void installPlatformExtensions(engine::Engine& engine);

// ---------------------------------------------------------------------------
// sessionStorage
// ---------------------------------------------------------------------------
Value makeSessionStorageValue();

// ---------------------------------------------------------------------------
// Element cloning hook & document tracking
// ---------------------------------------------------------------------------
void fireElementCloned(dom::Document* doc, dom::Element* src, dom::Element* clone);
Value hostDocumentValue(dom::Document* doc);
void clearHostDocument(dom::Document* doc);
void clearHostTimersForDocument(dom::Document* doc);
void clearHostAnimationFramesForDocument(dom::Document* doc);
bool hasPendingAnimationFrames();
void resetWindowHostOpenState();
dom::Document* currentHostDocument();
void setCurrentHostDocument(dom::Document* doc);
void deliverHostMediaQueryChanges();

} // namespace bro::bronze_host
