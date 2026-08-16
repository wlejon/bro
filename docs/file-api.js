// =============================================================================
// File API Reference — Blob, File, FileReader, object URLs, dropped files
// =============================================================================
//
// This is the surface an app uses to take a file *in*: the user drops one on
// the window (or the app builds bytes itself), and something has to turn that
// into a texture, a model, a document. On the web that is Blob/File + either
// FileReader or an object URL, and both halves have to work or the import path
// does not exist.
//
// Where the pieces live:
//   - `Blob` / `File`         brokit (C++, bytes held natively)
//   - `FileReader`            src/js/js/file_polyfills.js
//   - `URL.createObjectURL`   src/js/js/file_polyfills.js + src/util/object_url.h
//   - dropped-file payloads   src/js/event_dispatch_populate.cpp
//
// Related: docs/imagebitmap-api.js (createImageBitmap accepts a Blob),
// docs/brokit-api.js (fetch, fs), docs/paths-api.js (real filesystem paths).
//
// =============================================================================


// -----------------------------------------------------------------------------
// Blob / File
// -----------------------------------------------------------------------------
//
//   new Blob(parts, { type })            parts: strings, ArrayBuffers,
//                                        TypedArrays, other Blobs
//   new File(parts, name, { type, lastModified })
//
// Both hold their bytes in C++. `blob.size`, `blob.type`, `blob.slice()`,
// `await blob.text()`, `await blob.arrayBuffer()` all work; `File` adds `name`
// and `lastModified`, and inherits the rest.

/** @example
 *   const blob = new Blob([bytes], { type: 'image/png' });
 *   const png  = await blob.arrayBuffer();
 */


// -----------------------------------------------------------------------------
// FileReader
// -----------------------------------------------------------------------------
//
// The event-driven half of the File API. `blob.text()` / `blob.arrayBuffer()`
// are newer and simpler, but a great deal of shipped import code is written
// against FileReader (three.js's editor reads every model it imports this way),
// so it is here in full.
//
//   const reader = new FileReader();
//   reader.addEventListener('load', e => use(e.target.result));
//   reader.readAsArrayBuffer(file);
//
// Methods: readAsArrayBuffer, readAsText(blob[, encoding]), readAsDataURL,
// readAsBinaryString, abort().
// Properties: result, error, readyState (EMPTY 0 / LOADING 1 / DONE 2).
// Events (also available as on<name>): loadstart, progress, load, loadend,
// error, abort — each a ProgressEvent carrying loaded/total.
//
// A read always completes in a later turn, as on the web, so assigning the
// handler after calling read() is fine. Reading while a read is in flight
// throws.

/** @example
 *   function readModel(file) {
 *     return new Promise((resolve, reject) => {
 *       const r = new FileReader();
 *       r.onload  = () => resolve(r.result);
 *       r.onerror = () => reject(r.error);
 *       r.readAsArrayBuffer(file);
 *     });
 *   }
 */


// -----------------------------------------------------------------------------
// Object URLs
// -----------------------------------------------------------------------------
//
//   const url = URL.createObjectURL(blob);   // "blob:bro/17"
//   URL.revokeObjectURL(url);
//
// An object URL names bytes the page already holds so a URL consumer can read
// them. In bro they resolve everywhere a URL does:
//
//   - `<img src="blob:…">` and `new Image()` — sized and painted from the bytes
//   - `fetch(url)` — a 200 whose Content-Type is the Blob's type
//   - anything built on those, including three.js's TextureLoader/FileLoader
//
// The bytes are copied out of the Blob when the URL is created (they have to
// be readable from the render threads, which have no JS context), so a large
// blob costs its size again until the URL is revoked. Revoke when done —
// consumers already mid-read finish with the copy they hold.

/** @example
 *   // Resolve a model's texture that only exists in memory.
 *   const url = URL.createObjectURL(textureFile);
 *   new THREE.TextureLoader().load(url, tex => {
 *     material.map = tex;
 *     URL.revokeObjectURL(url);
 *   });
 */


// -----------------------------------------------------------------------------
// Dropped files
// -----------------------------------------------------------------------------
//
// A file dropped on the window fires dragenter → dragover → drop, once for the
// whole gesture however many files it carries. `event.dataTransfer` has:
//
//   .files      an array of real `File` objects — size, type, and bytes, so
//               FileReader / createObjectURL / createImageBitmap all take them
//   .types      ['Files'] for a file drop, plus 'text/plain' for dragged text
//   .getData(t) the dragged text, if any
//
// Each File also carries a non-standard `.path`: the real filesystem location
// it came from. A drop is the one moment a page is handed one, and bro apps can
// use real paths (see docs/paths-api.js) — reach for it when you want to read
// the file with `fs` or hand it to a sidecar tool instead of copying bytes.
//
// A path that cannot be read (a dropped directory, a permission error) arrives
// as a plain `{ name, path }` object instead of a File, so a drop never fails
// outright — check `instanceof File` if it matters.
//
// In headless, `dropFiles(x, y, paths)` synthesizes the whole gesture
// (docs/headless.md).

/** @example
 *   document.addEventListener('drop', async (event) => {
 *     event.preventDefault();
 *     if (event.dataTransfer.types[0] !== 'Files') return;
 *     for (const file of event.dataTransfer.files) {
 *       const text = await file.text();          // or a FileReader
 *       console.log(file.name, file.size, 'from', file.path, text.length);
 *     }
 *   });
 *
 *   document.addEventListener('dragover', e => e.preventDefault());
 */


// -----------------------------------------------------------------------------
// <input type=file>
// -----------------------------------------------------------------------------
//
// The other way in. Clicking a file input — or calling `.click()` on a hidden
// one from your own button, which is how nearly every page does it — opens the
// native picker, and what the user chose reads back as `input.files`: the same
// real `File` objects a drop produces, `.path` included.
//
//   <input type="file" accept=".obj,.gltf" multiple>
//
//   input.addEventListener('change', () => {
//     for (const file of input.files) load(file);
//   });
//
// `accept` filters the picker (extensions, MIME types, and the `image/*`-style
// wildcards); `multiple` allows more than one. Cancelling selects nothing and
// fires no events, leaving any earlier selection in place. `input.value` reads
// as the browser's fake path ("C:\fakepath\model.obj") — the real one is on
// each File.
//
// In headless there is no picker to open: `setPickedFiles(paths)` queues what
// the next click will choose, and the click consumes it (docs/headless.md).

/** @example
 *   // The standard hidden-input pattern.
 *   const input = document.createElement('input');
 *   input.type = 'file';
 *   input.accept = 'image/*';
 *   input.addEventListener('change', () => useTexture(input.files[0]));
 *   myButton.onclick = () => input.click();
 */


// -----------------------------------------------------------------------------
// Downloading — <a download>
// -----------------------------------------------------------------------------
//
// The other direction: handing a file *out*. There is exactly one way a page
// can do it, and every "Export" / "Save as" button on the web is built from it:
//
//   const url = URL.createObjectURL(new Blob([bytes], { type }));
//   const link = document.createElement('a');
//   link.href = url;
//   link.download = 'model.obj';
//   link.click();                 // or link.dispatchEvent(new MouseEvent('click'))
//   URL.revokeObjectURL(url);
//
// The file lands in the user's Downloads folder, named by the `download`
// attribute (or the URL's own last path segment when the attribute is empty).
// An existing file is never overwritten — "model (2).obj" and so on are tried.
// The attribute names a file, never a path: separators in it are replaced, so
// a page cannot write outside that folder.
//
// The href can be an object URL, a `data:` URL, or an app path — anything the
// runtime can read bytes from. `preventDefault()` on the click cancels the
// download, and an anchor without a `download` attribute is a plain link and
// saves nothing.
//
// An app that wants the user to choose the location has `showSaveFileDialog()`
// instead (docs/dialogs-api.js); this path is for code written against the web.
//
// In headless, `lastDownload()` returns the absolute path the most recent
// download wrote, so a test can assert on an export without knowing where the
// user's Downloads folder is (docs/headless.md).

/** @example
 *   function exportScene(text) {
 *     const url = URL.createObjectURL(new Blob([text], { type: 'text/plain' }));
 *     const link = document.createElement('a');
 *     link.href = url;
 *     link.download = 'scene.json';
 *     link.click();
 *     URL.revokeObjectURL(url);
 *   }
 */
