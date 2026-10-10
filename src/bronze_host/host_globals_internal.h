#pragma once

#include "bronze_host/host_document.h"
#include <include/core/SkImage.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bro::engine { class Engine; }
namespace bro::dom { class Element; class Node; }
namespace bro::render { struct DecodedImage; class GpuImageUpload; }

namespace bro::bronze_host {

// ---------------------------------------------------------------------------
// ImageBitmap & ImageData
// ---------------------------------------------------------------------------

// An ImageBitmap is immutable, so its pixels are a shared, read-only buffer:
// a clone (structured clone, createImageBitmap(bitmap) uncropped) shares it,
// a transfer moves the reference, and nothing on the page thread copies it.
// In the page realm a big bitmap also starts its texture upload the moment it
// exists (render/gpu_image_upload.h), so the frame that first draws it finds
// the texture made.
struct HostImageBitmap {
    uint32_t tag = 0x4849424D; // 'HIBM'
    sk_sp<SkImage> image;  // raster, over `pixels` (no copy)
    std::shared_ptr<const render::DecodedImage> pixels;  // straight-alpha RGBA8, width*height*4
    std::shared_ptr<render::GpuImageUpload> upload;      // page realm, big bitmaps only
    int width = 0;
    int height = 0;
    bool closed = false;

    const uint8_t* rgba() const;  // null when closed / empty
    // close(), a transfer, transferFromImageBitmap: detached, and everything
    // it held let go now rather than when the wrapper is collected.
    void detach();
};

const HostImageBitmap* hostImageBitmapOf(Value v);
HostImageBitmap* hostImageBitmapOfMut(Value v);
Value wrapHostImageBitmap(sk_sp<SkImage> img);
Value wrapHostImageBitmap(const uint8_t* rgba, int w, int h);
// Over `pixels` as they are: a transferred bitmap arriving, or a bitmap whose
// pixels were made for it. No copy. `upload`: the texture upload its sender
// started (startTransferUpload), adopted in the page realm.
Value wrapHostImageBitmap(std::shared_ptr<const render::DecodedImage> pixels,
                          std::shared_ptr<render::GpuImageUpload> upload = nullptr);
// A bitmap about to be transferred out of a worker: a big one starts its
// texture upload now, so it overlaps the message's hop to the page. Its own
// upload when it has one; null on the page thread or for a small bitmap.
std::shared_ptr<render::GpuImageUpload> startTransferUpload(const HostImageBitmap& bmp);
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
// The HTMLElement/Element constructor body. `newObject` is the receiver the
// `new` produced, which is what names the class when this is a direct
// `new MyElement()` rather than an upgrade.
Value constructCustomElementBase(Value newObject);

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
// window (dom_window.cpp)
// ---------------------------------------------------------------------------
void installWindowGlobal(engine::Engine* engine);
void clearWindowListeners();

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
