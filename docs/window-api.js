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

/**
 * Quits the app: the run loop stops at the top of the next frame, as when the
 * main window is closed. A no-op in `bro-headless` (the script's end is the
 * exit there), so an app's Quit button cannot end a test.
 */
bro.quit = function() {};

