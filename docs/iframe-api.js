// =============================================================================
// bro <iframe>, embedded, isolated sub-documents
// =============================================================================
//
// An <iframe src="..."> embeds a full, isolated bro app inside a box in the
// host document. It is not a web browsing context in the HTML sense. There is
// no network, no history, no same-origin policy, but it IS a real, live,
// isolated sub-document:
//
//   • its own JS realm (a separate JSContext, globals do NOT leak either way)
//   • its own DOM tree, CSS cascade, layout, and timers/requestAnimationFrame
//   • its own <canvas> scenes (2D), images, and ImageBitmaps
//   • rendered to its own GPU surface and composited at the element's box,
//     in DOM order, interleaved with the host's own canvas/WebGL/scene layers
//   • mouse input hit-tested and dispatched into the sub-document, with its
//     own :hover / :active / focus and click/dblclick semantics
//
// `src` points at a directory (or an index.html inside one): an iframe hosts a
// directory-based bro app, index.html + styles + scripts, exactly like a
// top-level `bro <dir>`. Classic scripts run; <script type="module"> is not
// yet supported inside an iframe and is skipped with a warning.
//
// This is the "open a document and render it" primitive: the host writes (or an
// agent generates) an app on disk, points an iframe at it, and the same render
// that shows the user is what the host can look at.
// =============================================================================


// -----------------------------------------------------------------------------
// Markup
// -----------------------------------------------------------------------------
//
// width/height attributes set the intrinsic box size (like <canvas>); CSS
// width/height override them. Everything else is normal CSS.
//
//   <iframe id="stage" src="./project/" width="640" height="480"></iframe>
//
//   iframe { border: 2px solid #444; border-radius: 8px; }
//
// The element's CONTENT BOX is the sub-document's viewport, and the
// sub-document tracks it: resizing the element re-lays-out the sub-document,
// re-evaluates its CSS @media rules, refreshes innerWidth/innerHeight in its
// realm, and fires 'resize' there (plus 'change' on any matchMedia list whose
// state flipped). See docs/matchmedia-api.js.

const frame = document.querySelector('#stage');


// -----------------------------------------------------------------------------
// Created from script
// -----------------------------------------------------------------------------
//
// An <iframe> does not have to come from the app's initial HTML, create one and
// append it, and its sub-document is built on the next frame (in headless, on the
// next flush()). Removing the element tears the sub-document down: JS realm, DOM,
// timers, canvas scenes, and GPU surface.
//
//   const f = document.createElement('iframe');
//   f.addEventListener('load', () => console.log('embedded app is up'));
//   f.src = './project/';
//   document.body.appendChild(f);
//   ...
//   f.remove();                       // sub-document destroyed
//
// A src that names neither a directory nor a file is an error: the iframe stays
// empty (capture() returns null) and the failure is logged once. It is NOT
// retried on every DOM change, assign src again, or call reload(), to retry.


// -----------------------------------------------------------------------------
// frame.src, get / set
// -----------------------------------------------------------------------------
//
// The getter reflects the src attribute. Assigning src (re)loads the embedded
// sub-document from the new location: the old sub-document is torn down (its JS
// realm, DOM, timers, and GPU surface released) and a fresh one is built.

console.log(frame.src);          // "./project/"
frame.src = './other-project/';  // tears down + loads the new app


// -----------------------------------------------------------------------------
// frame.reload()
// -----------------------------------------------------------------------------
//
// Rebuild the sub-document from its current src. This is the create → look →
// refine hook: rewrite the embedded app's files on disk, then reload() to
// re-render them. Cheaper to call than reassigning src to the same value; both
// do the same teardown + rebuild.

const fs = require('fs');
fs.writeFileSync('./project/index.html', newHtml);
frame.reload();

// The sub-document can also reload ITSELF: location.reload() inside the
// embedded app queues the same deferred teardown + rebuild of its own iframe.
// Deferred means the calling script keeps running to completion, the realm is
// torn down at the engine's next safe point, never re-entrantly, and multiple
// requests in one frame coalesce into a single rebuild. The host sees another
// 'load' event, exactly as if it had called frame.reload().
// (In the TOP-LEVEL document, location.reload() likewise tears down the app's
// document + JS realm and re-parses/re-runs the app in the same window.)


// -----------------------------------------------------------------------------
// frame.capture(), read back the rendered pixels ("look")
// -----------------------------------------------------------------------------
//
// Returns an ImageData ({ width, height, data:Uint8ClampedArray }, top-down
// RGBA) of exactly what the sub-document last rendered into its box, the same
// frame the user sees. This is the host's "look": generate an app, point an
// iframe at it, let a frame render, then read what it produced and hand it to
// an encoder or a vision model.
//
// capture() is authoritative, not a passive readback of whatever the compositor
// happens to be holding: it quiesces the raster worker, drains any queued
// reload, re-records the sub-document at its current box, and renders it on the
// host thread. So reload() + capture() returns the JUST-WRITTEN app on the FIRST
// call, no rAF timing games, no "wait for a frame to land" dance.
//
// (On the GPU path that render targets the sub-document's own surface; under
// --no-gpu it goes through the CPU rasterizer instead.)

frame.addEventListener('load', () => {
    const shot = frame.capture();            // ImageData
    if (shot) {
        const jpeg = bro.image.encodeJpeg(shot);   // → bytes for a vision model
        // ...send jpeg to the model, critique, rewrite files, frame.reload()
    }
});


// -----------------------------------------------------------------------------
// 'load' event
// -----------------------------------------------------------------------------
//
// Fires on the <iframe> element (host realm) once the sub-document has been
// parsed, scripted, and laid out, i.e. when it is safe to look at or drive the
// embedded app. Fires on the initial load and on every reload()/src assignment.
// Does not bubble.

frame.addEventListener('load', () => {
    console.log('embedded app ready');
    // ...inspect the rendered frame, drive it, screenshot it, etc.
});


// -----------------------------------------------------------------------------
// Input
// -----------------------------------------------------------------------------
//
// Mouse events that land on the iframe are translated into the sub-document's
// own coordinate space and dispatched there: the embedded app sees mousedown /
// mouseup / click / mousemove, resolves its own :hover, and runs its own
// listeners. The host does NOT see those events as clicks on the <iframe>
// element, the frame is opaque to the host, like a real embedded document.
//
// (Keyboard/focus routing and wheel routing into sub-documents are not yet
//  wired, a follow-up.)


// -----------------------------------------------------------------------------
// Isolation notes
// -----------------------------------------------------------------------------
//
//   • globalThis inside the iframe is a different object than the host's;
//     nothing set on one is visible on the other.
//   • There is no contentWindow / contentDocument accessor yet: the host cannot
//     reach into the sub-document's DOM or JS from script. Drive it via files +
//     reload(), and via input.
//   • Nesting is NOT supported (v1): an <iframe> inside an iframe app never gets
//     a sub-document, the element renders as an empty box. Only the app
//     document is walked for frames. A nested frame under a bro.window host logs
//     a warning; nested inside another iframe it fails silently.
