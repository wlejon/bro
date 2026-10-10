#pragma once

// An <img>'s load, off the page thread (render/image_store.h has the store).
//
// Setting `src` — through the `src` property, setAttribute, innerHTML or the
// page's own markup — resolves the URL (no I/O beyond a stat), takes the
// store's request for those bytes, and returns:
//   * a request already decoded (another element, the painter, or an earlier
//     load of the same file asked for it) makes the image available at once:
//     natural size, `complete`, and `load` queued as a task, as a browser's
//     list of available images does;
//   * a request still decoding leaves the image incomplete, its natural size
//     taken from the file's header where that is cheap (a file, inline bytes)
//     so layout gives the box its size before the pixels land; the pump below
//     settles it when the decoder is done;
//   * nothing to read (a missing file, a revoked blob:) is a broken image:
//     zero size, `complete`, and `error` queued.
// The painter draws nothing for an image still decoding and repaints when it
// lands (pumpImageLoads marks the documents' paint stale).
//
// Main thread only, all of it; the decoding is the store's.

#include <functional>
#include <string>

namespace bro::dom { class Element; }

namespace bro::layout {

// Begin (or reuse) the load of `src` for the <img> `el`.
void loadImageElement(dom::Element* el, const std::string& src);

// Settle every load whose decode finished: natural size, `complete`, the
// load / error event (a task), settle callbacks, and a repaint of every
// document (a background or border-image may have been waiting too). Cheap
// when nothing settled. Once a frame, and from headless flush(). True when
// anything settled since the last pump.
bool pumpImageLoads();

// A settle the next pump has to deliver: the main loop must not idle on it.
bool imageSettlesPending();

// Headless runs settle loads deterministically: wait (at most `timeoutMs`)
// for the decodes in flight, then pump.
void settleImageLoads(double timeoutMs = 10000.0);

// Run `cb(ok)` when `el`'s current load settles — on the pump that settles it
// (after its load / error event is queued), or now when it has no load in
// flight. A load superseded by a new src settles its waiters with false.
void whenImageSettled(dom::Element* el, std::function<void(bool ok)> cb);

// Headless paints wait for the decodes they need rather than painting the
// gap, so a screenshot is deterministic. Off (the default) everywhere else.
void setPaintWaitsForImages(bool wait);
bool paintWaitsForImages();

}  // namespace bro::layout
