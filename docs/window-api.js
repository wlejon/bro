/**
 * =============================================================================
 * bro.window & window.* — Runtime Window & Display Management
 * =============================================================================
 *
 * Runtime window state control (borderless, always-on-top, position, size limits,
 * display enumeration and placement).
 *
 * @example
 *   bro.window.borderless = true;
 *   bro.window.alwaysOnTop = true;
 *   const pos = bro.window.getPosition();
 *   console.log('Window position:', pos.x, pos.y);
 *   const displays = bro.window.getDisplays();
 *   console.log('Displays attached:', displays.length);
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * Desktop coordinates and dimensions of a rectangle.
 * @typedef {Object} DisplayRect
 * @property {number} [x] -  X coordinate in desktop pixels.
 * @property {number} [y] -  Y coordinate in desktop pixels.
 * @property {number} [width] -  Width in desktop pixels.
 * @property {number} [height] -  Height in desktop pixels.
 */

/**
 * Display device descriptor.
 * @typedef {Object} DisplayInfo
 * @property {number} [id] -  Stable SDL display identifier.
 * @property {string} [name] -  Display device name.
 * @property {number} [x] -  X coordinate in desktop pixels.
 * @property {number} [y] -  Y coordinate in desktop pixels.
 * @property {number} [width] -  Width in desktop pixels.
 * @property {number} [height] -  Height in desktop pixels.
 * @property {number} [workX] -  Usable work area X in desktop pixels.
 * @property {number} [workY] -  Usable work area Y in desktop pixels.
 * @property {number} [workWidth] -  Usable work area width in desktop pixels.
 * @property {number} [workHeight] -  Usable work area height in desktop pixels.
 * @property {number} [refreshRate] -  Refresh rate in Hz.
 * @property {number} [contentScale] -  OS content scale multiplier (1.0 = 100%).
 * @property {boolean} [isPrimary] -  Whether this is the system primary display.
 * @property {boolean} [isCurrent] -  Whether the active window currently sits on this display.
 * @property {DisplayRect} [bounds] -  The same rectangle as x/y/width/height, nested.
 * @property {DisplayRect} [workArea] -  The same rectangle as workX/workY/workWidth/workHeight, nested.
 */

/**
 * 2D desktop position.
 * @typedef {Object} WindowPosition
 * @property {number} [x] -  Desktop X coordinate.
 * @property {number} [y] -  Desktop Y coordinate.
 */

/**
 * 2D window dimensions.
 * @typedef {Object} WindowSize
 * @property {number} [width] -  Width in pixels.
 * @property {number} [height] -  Height in pixels.
 */

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * Runtime window management namespace.
 */
/**
 * Current window display state ('normal', 'minimized', 'maximized', 'fullscreen').
 * @readonly
 * @type {string}
 */
bro.window.state;

/**
 * Whether the window has OS borders and title bar removed.
 * @type {boolean}
 */
bro.window.borderless;

/**
 * Whether the window stays pinned above standard windows.
 * @type {boolean}
 */
bro.window.alwaysOnTop;

/**
 * Minimizes the window.
 */
bro.window.minimize = function() {};

/**
 * Maximizes the window.
 */
bro.window.maximize = function() {};

/**
 * Restores the window from minimized or maximized state.
 */
bro.window.restore = function() {};

/**
 * Retrieves current desktop coordinate position of the window.
 * @returns {WindowPosition}
 */
bro.window.getPosition = function() {};

/**
 * @param {number} x
 * @param {number} y
 */
bro.window.setPosition = function(x, y) {};

/**
 * @returns {WindowSize}
 */
bro.window.getMinSize = function() {};

/**
 * @param {number} width
 * @param {number} height
 */
bro.window.setMinSize = function(width, height) {};

/**
 * @returns {WindowSize}
 */
bro.window.getMaxSize = function() {};

/**
 * @param {number} width
 * @param {number} height
 */
bro.window.setMaxSize = function(width, height) {};

/**
 * @returns {Array<DisplayInfo>}
 */
bro.window.getDisplays = function() {};

/**
 * @param {number} id
 * @returns {boolean}
 */
bro.window.moveToDisplay = function(id) {};

/**
 * Current client-area size of the main window. Windowed, this is the live OS
 * window size (so it reads the new size straight after `setSize`); headless,
 * it is the virtual viewport.
 * @returns {WindowSize}
 */
bro.window.getSize = function() {};

/**
 * Resizes the main window's client area. Windowed, the OS window is resized
 * and the document relays out / fires `resize` when the OS confirms it;
 * headless, the virtual viewport is resized immediately (relayout and the
 * `resize` event happen inside the call, as with the headless `resize()`
 * helper). Throws `RangeError` for a size below 1x1.
 * @param {number} width
 * @param {number} height
 *
 * @example
 *   bro.window.setSize(1280, 720);
 */
bro.window.setSize = function(width, height) {};

/**
 * Web-compatible spelling of `bro.window.setSize(width, height)` for the main window.
 * @param {number} width
 * @param {number} height
 */
window.resizeTo = function(width, height) {};

/**
 * Grows (or shrinks, with negative deltas) the main window by `dx`, `dy`
 * relative to `bro.window.getSize()`.
 * @param {number} dx
 * @param {number} dy
 */
window.resizeBy = function(dx, dy) {};

// ── Secondary windows: bro.window.open / window.open ────────────────────────
//
// A secondary window hosts another app directory (its own index.html, its own
// realm and DOM) in a second OS window. open() is queued: the OS window and
// its document materialize at the engine's next idle drain (flush() in
// headless), which is when 'load' fires. Headless windows are always hidden;
// everything else — the document, capture(), messaging, input routed by
// window id — works the same. Only the main app realm may open one.

/**
 * Open a secondary window on the app directory `src` (relative to the app).
 * The child's bro.json fills in any option left unset.
 * @param {string} src
 * @param {Object} [opts]  width, height, title, x, y, display (index into
 *   getDisplays()), resizable, borderless, alwaysOnTop, minWidth, minHeight,
 *   maxWidth, maxHeight
 * @returns {BroWindowHandle}  throws TypeError on a missing/empty src, from a
 *   child realm, or when there is no primary window (Server mode).
 */
bro.window.open = function(src, opts) {};

/**
 * The handle open() returns.
 * @typedef {Object} BroWindowHandle
 * @property {number} id        routes headless input: click(x, y, 0, win.id)
 * @property {boolean} closed   true once closed, by close(), the OS, or a src
 *                              that failed to load (it closes at the drain)
 * getSize() / setSize(w, h) / getPosition() / setPosition(x, y) (a no-op on a
 * hidden window) / setTitle(s) / focus() / close()
 * capture() -> ImageData of the window's rendered document, or null
 * postMessage(data, targetOrigin | options | transfer) -> a MessageEvent at
 *   the child's window (events-api.js)
 * addEventListener / removeEventListener for 'load', 'close', 'resize',
 *   'message' (messages the child posts with window.opener.postMessage or
 *   bro.window.parent.postMessage)
 */

/**
 * The web spelling. `url` with a scheme of its own (https:, mailto:, file:,
 * ...) is handed to the OS handler (browser, mail client) via SDL_OpenURL and
 * returns null; headless never shells out. An app-relative `url` opens a bro
 * window exactly as bro.window.open does and returns its handle — in headless
 * too. `target` names the window (its title), except the `_blank`/`_self`
 * keywords; `features` is either "width=400,height=300,left=..,top=.." or an
 * { width, height } object. With `noopener`/`noreferrer` in `features` the
 * window still opens and the call returns null. Empty / about:blank / a call
 * from a child realm returns null.
 * @param {string} [url]
 * @param {string} [target]
 * @param {string|Object} [features]
 * @returns {BroWindowHandle|null}
 *
 * @example
 *   const palette = window.open('palette', 'Palette', 'width=300,height=200');
 *   window.open('https://example.com');   // default browser; returns null
 */
window.open = function(url, target, features) {};

/**
 * Quits the app: the run loop stops at the top of the next frame, as when the
 * main window is closed. A no-op in `bro-headless` (the script's end is the
 * exit there), so an app's Quit button cannot end a test.
 */
bro.quit = function() {};

