/**
 * =============================================================================
 * HTMLIFrameElement (<iframe>) — Embedded Isolated Sub-Documents
 * =============================================================================
 *
 * Implements the HTMLIFrameElement interface for embedding isolated bro app
 * sub-documents with independent JS realms, DOM trees, CSS cascades, timers,
 * and GPU surfaces.
 *
 * A frame is a realm of its own (its own globals, builtins frozen as the
 * page's are), and it is the page's own app: the page reaches its window and
 * document and the frame reaches `parent`, `top` and `frameElement`, as the
 * web's same-origin frames do, and a function of either runs in its own realm
 * whoever calls it. Another app running inline is an `<appview>`
 * (docs/appview-api.js), reached only by message.
 *
 * The sub-document's `<script>` tags run in document order as the main
 * document's do, `<script type="module">` included: inline and `src=` modules
 * resolve their relative imports against the sub-document's folder, and a
 * module may use top-level `await`.
 *
 * A `src` folder outside the app's own folders is read as `fs` would read it
 * (docs/apps.md, Permissions): without `files` it is refused (logged) and no
 * sub-document loads. The same holds for the page's other subresources from
 * disk: an `<img>` or CSS url() (the `error` event), a `<video>` (a failed
 * load), an `@font-face` src or an `@import`. They are answered from what is
 * decided and never raise a prompt.
 *
 * @example
 *   // Create dynamic iframe and capture its rendered output
 *   const frame = document.createElement('iframe');
 *   frame.src = './embedded_app/';
 *   frame.addEventListener('load', () => {
 *     const image = frame.capture();
 *     console.log('Captured frame dimensions:', image.width, image.height);
 *   });
 *   document.body.appendChild(frame);
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * Embedded sub-document element interface.
 */
class HTMLIFrameElement extends HTMLElement {

  /**
   *  Source path or directory for embedded sub-document.
   * @type {string}
   */
  src;

  /**
   *  Intrinsic width of iframe element.
   * @type {string}
   */
  width;

  /**
   *  Intrinsic height of iframe element.
   * @type {string}
   */
  height;

  /**
   *  Document of embedded sub-document (if accessible).
   * @readonly
   * @type {*}
   */
  contentDocument;

  /**
   *  Window global proxy of embedded sub-document.
   * @readonly
   * @type {*}
   */
  contentWindow;

  /**
   *  Rebuilds and reloads sub-document from its current src.
   */
  reload() {}

  /**
   *  Synchronously captures rendered pixels of sub-document as an ImageData.
   * @returns {Object|null}
   */
  capture() {}

}

