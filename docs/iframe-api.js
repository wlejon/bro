/**
 * =============================================================================
 * HTMLIFrameElement (<iframe>) — Embedded Isolated Sub-Documents
 * =============================================================================
 *
 * Implements the HTMLIFrameElement interface for embedding isolated bro app
 * sub-documents with independent JS realms, DOM trees, CSS cascades, timers,
 * and GPU surfaces.
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

