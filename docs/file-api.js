/**
 * =============================================================================
 * File API Reference — Blob, File, FileReader, URL, Object URLs, Dropped Files
 * =============================================================================
 *
 * This is the surface an app uses to take a file *in*: the user drops one on
 * the window (or the app builds bytes itself), and something has to turn that
 * into a texture, a model, a document. On the web that is Blob/File + either
 * FileReader or an object URL, and both halves have to work or the import path
 * does not exist.
 *
 * Where the pieces live:
 *   - `Blob` / `File`         brokit (C++, bytes held natively)
 *   - `FileReader`            src/js/js/file_polyfills.js & bronze_host
 *   - `URL.createObjectURL`   src/js/js/file_polyfills.js + src/util/object_url.h
 *   - dropped-file payloads   src/js/event_dispatch_populate.cpp
 *
 * @example
 *   // --- Blob / File Example ------------------------------------------------
 *   const blob = new Blob([bytes], { type: 'image/png' });
 *   const png  = await blob.arrayBuffer();
 *
 * @example
 *   // --- FileReader Example -------------------------------------------------
 *   function readModel(file) {
 *     return new Promise((resolve, reject) => {
 *       const r = new FileReader();
 *       r.onload  = () => resolve(r.result);
 *       r.onerror = () => reject(r.error);
 *       r.readAsArrayBuffer(file);
 *     });
 *   }
 *
 * @example
 *   // --- Object URLs Example ------------------------------------------------
 *   // Resolve a model's texture that only exists in memory.
 *   const url = URL.createObjectURL(textureFile);
 *   new THREE.TextureLoader().load(url, tex => {
 *     material.map = tex;
 *     URL.revokeObjectURL(url);
 *   });
 *
 * @example
 *   // --- Dropped Files Example ----------------------------------------------
 *   document.addEventListener('drop', async (event) => {
 *     event.preventDefault();
 *     if (event.dataTransfer.types[0] !== 'Files') return;
 *     for (const file of event.dataTransfer.files) {
 *       const text = await file.text();          // or a FileReader
 *       console.log(file.name, file.size, 'from', file.path, text.length);
 *     }
 *   });
 *   document.addEventListener('dragover', e => e.preventDefault());
 *
 * @example
 *   // --- Input File Example -------------------------------------------------
 *   // The standard hidden-input pattern.
 *   const input = document.createElement('input');
 *   input.type = 'file';
 *   input.accept = 'image/*';
 *   input.addEventListener('change', () => useTexture(input.files[0]));
 *   myButton.onclick = () => input.click();
 *
 * @example
 *   // --- Downloading Example (<a download>) ---------------------------------
 *   function exportScene(text) {
 *     const url = URL.createObjectURL(new Blob([text], { type: 'text/plain' }));
 *     const link = document.createElement('a');
 *     link.href = url;
 *     link.download = 'scene.json';
 *     link.click();
 *     URL.revokeObjectURL(url);
 *   }
 *
 * @example
 *   // --- Dragging inside page (DataTransfer) --------------------------------
 *   row.draggable = true;
 *   row.addEventListener('dragstart', e => e.dataTransfer.setData('text/plain', row.id));
 *   list.addEventListener('dragover',  e => e.preventDefault());   // required
 *   list.addEventListener('drop', e => {
 *     e.preventDefault();
 *     const moved = document.getElementById(e.dataTransfer.getData('text/plain'));
 *     const before = e.offsetY < e.target.clientHeight / 2;
 *     e.target.parentNode.insertBefore(moved, before ? e.target : e.target.nextSibling);
 *   });
 */

// ── Typedefs ─────────────────────────────────────────────────────────────────

/**
 * Union of valid parts that can compose a Blob.
 * @type {(string|ArrayBuffer|ArrayBufferView|Blob)}
 */
// typedef (string|ArrayBuffer|ArrayBufferView|Blob) BlobPart;

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * Initialization dictionary for creating a Blob.
 * @typedef {Object} BlobPropertyBag
 * @property {string} [type=""]
 */

/**
 * Initialization dictionary for creating a File.
 * @typedef {Object} FilePropertyBag extends BlobPropertyBag
 * @property {number} [lastModified]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * Represents immutable raw binary data held in native memory.
 */
class Blob {

  /**
   * Create a new Blob from an array of parts (strings, ArrayBuffers, TypedArrays, or Blobs).
   *
   * @param {Array<BlobPart>} [blobParts] - Array of parts to concatenate into the blob
   * @param {BlobPropertyBag} [options] - Configuration options including MIME type
   */
  constructor(blobParts, options) {}

  /**
   * Byte length of the blob data.
   * @readonly
   * @type {number}
   */
  size;

  /**
   * Normalized ASCII-encoded lowercase MIME type string (e.g. "image/png").
   * @readonly
   * @type {string}
   */
  type;

  /**
   * Returns a new Blob object containing the data in the specified byte range.
   *
   * @param {number} [start] - Byte offset start (negative offsets count from end)
   * @param {number} [end] - Byte offset end
   * @param {string} [contentType] - MIME type of the returned slice
   * @returns {Blob} Sliced Blob instance
   */
  slice(start, end, contentType) {}

  /**
   * Asynchronously reads and decodes the blob contents as a UTF-8 text string.
   * @returns {Promise<string>} Promise resolving to the text content
   */
  text() {}

  /**
   * Asynchronously reads the blob contents as a binary ArrayBuffer.
   * @returns {Promise<ArrayBuffer>} Promise resolving to an ArrayBuffer copy of the bytes
   */
  arrayBuffer() {}

  /**
   * Asynchronously returns a Uint8Array view of the blob's binary contents.
   * @returns {Promise<Uint8Array>} Promise resolving to Uint8Array
   */
  bytes() {}

}

/**
 * A File represents a Blob with file metadata (filename, last modified timestamp, and origin path).
 */
class File extends Blob {

  /**
   * Construct a File instance.
   *
   * @param {Array<BlobPart>} fileBits - Array of parts forming the file's binary content
   * @param {string} fileName - The name of the file
   * @param {FilePropertyBag} [options] - Options specifying MIME type and lastModified time
   */
  constructor(fileBits, fileName, options) {}

  /**
   * Name of the file.
   * @readonly
   * @type {string}
   */
  name;

  /**
   * Milliseconds timestamp since Unix epoch when the file was last modified.
   * @readonly
   * @type {number}
   */
  lastModified;

  /**
   * Relative path if picked via a directory picker, otherwise empty.
   * @readonly
   * @type {string}
   */
  webkitRelativePath;

  /**
   * Absolute real filesystem path when the file originates from a drag-and-drop gesture or native file picker.
   * @readonly
   * @type {string}
   */
  path;

}

/**
 * Event-driven async reader for Blob and File objects.
 */
class FileReader {

  /**
   * Construct a new FileReader.
   */
  constructor() {}

  /**
   * @readonly
   * @type {number}
   */
  static readonly EMPTY = 0;

  /**
   * @readonly
   * @type {number}
   */
  static readonly LOADING = 1;

  /**
   * @readonly
   * @type {number}
   */
  static readonly DONE = 2;

  /**
   * Current state of the reader (EMPTY 0, LOADING 1, DONE 2).
   * @readonly
   * @type {number}
   */
  readyState;

  /**
   * Result of the read operation (DOMString or ArrayBuffer on success, null on error/empty).
   * @readonly
   * @type {(string|ArrayBuffer|null)}
   */
  result;

  /**
   * Error object if the read failed, otherwise null.
   * @readonly
   * @type {Object|null}
   */
  error;

  /**
   * Event handler for read start.
   * @type {EventHandler}
   */
  onloadstart;

  /**
   * Event handler for progress updates.
   * @type {EventHandler}
   */
  onprogress;

  /**
   * Event handler for successful load completion.
   * @type {EventHandler}
   */
  onload;

  /**
   * Event handler for aborted read operations.
   * @type {EventHandler}
   */
  onabort;

  /**
   * Event handler for read errors.
   * @type {EventHandler}
   */
  onerror;

  /**
   * Event handler for completed read operations (success or failure).
   * @type {EventHandler}
   */
  onloadend;

  /**
   * Starts reading the contents of the specified Blob or File as an ArrayBuffer.
   *
   * @param {Blob} blob - The Blob to read
   */
  readAsArrayBuffer(blob) {}

  /**
   * Starts reading the contents of the specified Blob as a binary string (one character per byte).
   *
   * @param {Blob} blob - The Blob to read
   */
  readAsBinaryString(blob) {}

  /**
   * Starts reading the contents of the specified Blob as UTF-8 text.
   *
   * @param {Blob} blob - The Blob to read
   * @param {string} [encoding] - Optional encoding string (UTF-8 is default)
   */
  readAsText(blob, encoding) {}

  /**
   * Starts reading the contents of the specified Blob as a base64 data: URL.
   *
   * @param {Blob} blob - The Blob to read
   */
  readAsDataURL(blob) {}

  /**
   * Aborts the in-flight read operation and resets reader state.
   */
  abort() {}

  /**
   * Register an event listener for reader events.
   *
   * @param {string} type - Event type ('load', 'error', 'loadend', 'progress', 'abort')
   * @param {EventListener} listener - Event listener callback
   */
  addEventListener(type, listener) {}

  /**
   * Remove a previously registered event listener.
   *
   * @param {string} type - Event type
   * @param {EventListener} listener - Event listener callback
   */
  removeEventListener(type, listener) {}

}

/**
 * URL query parameter parser and manipulator.
 */
class URLSearchParams {

  /**
   * Create a URLSearchParams object from query string, entries, or dictionary.
   *
   * @param {(Array<Array<string>>|Object<string, string>|string)} [init]
   */
  constructor(init) {}

  /**
   * Appends a specified key/value pair as a new search parameter.
   *
   * @param {string} name
   * @param {string} value
   */
  append(name, value) {}

  /**
   * Deletes the given search parameter and its associated value(s).
   *
   * @param {string} name
   */
  delete(name) {}

  /**
   * Returns the first value associated to the given search parameter.
   *
   * @param {string} name
   * @returns {string|null}
   */
  get(name) {}

  /**
   * Returns all the values associated with a given search parameter as an array.
   *
   * @param {string} name
   * @returns {Array<string>}
   */
  getAll(name) {}

  /**
   * Returns a boolean indicating if such a search parameter exists.
   *
   * @param {string} name
   * @returns {boolean}
   */
  has(name) {}

  /**
   * Sets the value associated with a given search parameter to the given value.
   *
   * @param {string} name
   * @param {string} value
   */
  set(name, value) {}

  /**
   * Returns a query string suitable for use in a URL.
   * @returns {string}
   */
  toString() {}

}

/**
 * Standard WHATWG URL interface for parsing, constructing, and resolving URLs and Blob URLs.
 */
class URL {

  /**
   * Parse an absolute or relative URL string against an optional base URL.
   *
   * @param {string} url - Absolute or relative URL string
   * @param {string} [base] - Optional base URL string
   */
  constructor(url, base) {}

  /**
   * Mint an object URL naming the given Blob's bytes (e.g. "blob:bro/17").
   *
   * @param {Blob} obj - The Blob to expose
   * @returns {string} Object URL string
   */
  static createObjectURL(obj) {}

  /**
   * Revoke an object URL previously created with createObjectURL.
   *
   * @param {string} url - The object URL string to revoke
   */
  static revokeObjectURL(url) {}

  /**
   * Parse a URL string against a base, returning null on invalid input instead of throwing.
   *
   * @param {string} url - URL string to parse
   * @param {string} [base] - Optional base URL
   * @returns {URL|null} Parsed URL instance or null if invalid
   */
  static parse(url, base) {}

  /**
   * Full serialized URL href string.
   * @type {string}
   */
  href;

  /**
   * Origin of the URL (protocol + hostname + port).
   * @readonly
   * @type {string}
   */
  origin;

  /**
   * Protocol scheme of the URL (including trailing colon ':').
   * @type {string}
   */
  protocol;

  /**
   * Host (hostname and port) of the URL.
   * @type {string}
   */
  host;

  /**
   * Hostname without port number.
   * @type {string}
   */
  hostname;

  /**
   * Port number string.
   * @type {string}
   */
  port;

  /**
   * Pathname component of the URL.
   * @type {string}
   */
  pathname;

  /**
   * Query search string (including leading '?').
   * @type {string}
   */
  search;

  /**
   * URLSearchParams object representing query parameters.
   * @readonly
   * @type {URLSearchParams}
   */
  searchParams;

  /**
   * Fragment hash component (including leading '#').
   * @type {string}
   */
  hash;

  /**
   * Returns serialized URL href.
   * @returns {string}
   */
  toJSON() {}

  /**
   * Returns serialized URL href.
   * @returns {string}
   */
  toString() {}

}

