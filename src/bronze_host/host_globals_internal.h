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
void onCustomElementConnected(dom::Element* el);
void onCustomElementDisconnected(dom::Element* el);
void onCustomElementAttributeChanged(dom::Element* el, const std::string& name,
                                     const char* oldValue, const char* newValue);
Value constructCustomElement(dom::Element* el, const std::string& tagName);
Value constructCustomElementBase();

// ---------------------------------------------------------------------------
// structuredClone
// ---------------------------------------------------------------------------

void installStructuredCloneGlobals();

// ---------------------------------------------------------------------------
// Buffer
// ---------------------------------------------------------------------------

void installBufferGlobals();

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

void installWorkerGlobals(engine::Engine& engine);
void drainWorkerMessages();

// ---------------------------------------------------------------------------
// Combined extensions installer
// ---------------------------------------------------------------------------

void installPlatformExtensions(engine::Engine& engine);

} // namespace bro::bronze_host
