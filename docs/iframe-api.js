// =============================================================================
// bro <iframe> — embedded, isolated sub-documents
// =============================================================================
//
// An <iframe src="..."> embeds a full, isolated bro app inside a box in the
// host document. It is not a web browsing context in the HTML sense — there is
// no network, no history, no same-origin policy — but it IS a real, live,
// isolated sub-document:
//
//   • its own JS realm (a separate JSContext — globals do NOT leak either way)
//   • its own DOM tree, CSS cascade, layout, and timers/requestAnimationFrame
//   • its own <canvas> scenes (2D), images, and ImageBitmaps
//   • rendered to its own GPU surface and composited at the element's box,
//     in DOM order, interleaved with the host's own canvas/WebGL/scene layers
//   • mouse input hit-tested and dispatched into the sub-document, with its
//     own :hover / :active / focus and click/dblclick semantics
//
// `src` points at a directory (or an index.html inside one): an iframe hosts a
// directory-based bro app — index.html + styles + scripts — exactly like a
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

const frame = document.querySelector('#stage');


// -----------------------------------------------------------------------------
// frame.src  — get / set
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


// -----------------------------------------------------------------------------
// frame.capture()  — read back the rendered pixels ("look")
// -----------------------------------------------------------------------------
//
// Returns an ImageData ({ width, height, data:Uint8ClampedArray }, top-down
// RGBA) of exactly what the sub-document last rendered into its box — the same
// frame the user sees. This is the host's "look": generate an app, point an
// iframe at it, let a frame render, then read what it produced and hand it to
// an encoder or a vision model.
//
// Returns null if the sub-document hasn't rendered yet, so capture AFTER the
// 'load' event AND after a frame has been drawn (in a windowed app the render
// loop is continuous; in headless, call screenshot()/flush() to force a frame,
// or capture from within a requestAnimationFrame callback).
//
// The readback is a direct GL read of the sub-document's own GPU surface on the
// host thread — no copy back through JS of the whole DOM, and no cross-context
// stall.

frame.addEventListener('load', () => {
    requestAnimationFrame(() => {
        const shot = frame.capture();            // ImageData or null
        if (shot) {
            const jpeg = bro.image.encodeJpeg(shot);   // → bytes for a vision model
            // ...send jpeg to the model, critique, rewrite files, frame.reload()
        }
    });
});


// -----------------------------------------------------------------------------
// 'load' event
// -----------------------------------------------------------------------------
//
// Fires on the <iframe> element (host realm) once the sub-document has been
// parsed, scripted, and laid out — i.e. when it is safe to look at or drive the
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
// element — the frame is opaque to the host, like a real embedded document.
//
// (Keyboard/focus routing and wheel routing into sub-documents are not yet
//  wired — a follow-up.)


// -----------------------------------------------------------------------------
// Isolation notes
// -----------------------------------------------------------------------------
//
//   • globalThis inside the iframe is a different object than the host's;
//     nothing set on one is visible on the other.
//   • There is no contentWindow / contentDocument accessor yet: the host cannot
//     reach into the sub-document's DOM or JS from script. Drive it via files +
//     reload(), and via input.
//   • Nesting works (an iframe app may itself contain an <iframe>), each level a
//     fresh isolated sub-document.
