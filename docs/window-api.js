/**
 * =============================================================================
 * bro.window & window.* — Runtime Window & Display Management
 * =============================================================================
 *
 * Runtime window state control (borderless, always-on-top, position, size limits,
 * display enumeration and placement).
 *
 * `bro.window` drives the calling realm's own window: in a secondary window's
 * realm (bro.window.open) every read and write targets that window, so a child
 * reads back the flags and limits its bro.json declared. (`window.close()`
 * closes a secondary window from inside; `bro.quit()` quits the whole app.)
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
 * Window title text.
 * @type {string}
 */
bro.window.title;

/**
 * @returns {string}
 */
bro.window.getTitle = function() {};

/**
 * @param {string} title
 */
bro.window.setTitle = function(title) {};

/**
 * Window opacity from 0.0 (fully transparent) to 1.0 (fully opaque).
 * @type {number}
 */
bro.window.opacity;

/**
 * @returns {number}
 */
bro.window.getOpacity = function() {};

/**
 * @param {number} opacity
 */
bro.window.setOpacity = function(opacity) {};

/**
 * Whether the window is in fullscreen mode.
 * @type {boolean}
 */
bro.window.fullscreen;

/**
 * Toggles fullscreen display mode.
 * @returns {boolean}
 */
bro.window.toggleFullscreen = function() {};

/**
 * Whether the window currently has keyboard/input focus.
 * @readonly
 * @type {boolean}
 */
bro.window.focused;

/**
 * Flashes the taskbar/dock button to request user attention until focused.
 * @param {boolean} [on=true]
 * @returns {boolean}
 */
bro.window.flash = function(on) {};

/**
 * Alias for bro.window.flash(on).
 * @param {boolean} [on=true]
 * @returns {boolean}
 */
bro.window.requestAttention = function(on) {};

/**
 * Plays the system's default alert or bell sound.
 * @returns {boolean}
 */
bro.window.beep = function() {};

/**
 * Sets taskbar progress indicator state ('none', 'normal', 'error', 'indeterminate', 'paused')
 * and completion percentage (0 - 100).
 * @param {string|number} state
 * @param {number} [value=0]
 * @returns {boolean}
 */
bro.window.setProgress = function(state, value) {};

/**
 * Displays a desktop notification.
 * @param {string} title
 * @param {string} [body=""]
 * @param {Object} [options]
 * @param {string} [options.icon]
 * @param {number} [options.timeout]
 * @param {boolean} [options.silent]
 * @param {number} [options.replacesId]
 * @returns {number}
 */
bro.window.notify = function(title, body, options) {};

/**
 * Configures or updates the system tray icon with context menu.
 * @param {Object} options
 * @param {string} [options.icon]
 * @param {string} [options.tooltip]
 * @param {Array<Object>} [options.menu]
 * @returns {boolean}
 */
bro.window.setTray = function(options) {};

/**
 * Removes the system tray icon.
 * @returns {boolean}
 */
bro.window.removeTray = function() {};

/**
 * Whether a tray icon is currently active.
 * @returns {boolean}
 */
bro.window.hasTray = function() {};

/**
 * Whether the system tray is supported on this platform.
 * @returns {boolean}
 */
bro.window.isTrayAvailable = function() {};

/**
 * How bro is presenting: 'windowed' (an OS window), 'headless', 'server' (bro-server) or
 * 'drm' (bro --drm: bro is the display server and the app is the desktop shell; see
 * docs/compositor-api.js for how input is routed between the shell and client windows).
 * @type {string}
 * @readonly
 */
bro.window.displayMode;

/**
 * Captures what is on screen to a PNG (docs/screen-capture.md). Under DRM it is the frame bro
 * last handed to KMS scanout, read back from the scanout buffer: client windows, the shell and
 * its overlays exactly as the display shows them. Elsewhere it is the app's own composite.
 * Synchronous; under DRM it costs a read of the scanout buffer and a PNG encode.
 *
 * @param {string} [path] where to write the PNG; default `$XDG_RUNTIME_DIR/bro-screen.png`
 * @returns {string|null} the path written, or null (the reason is logged)
 */
bro.window.captureScreen('/tmp/desktop.png');

/**
 * Registers a global hotkey: a chord that reaches the app whichever window has the keyboard.
 *
 * Accelerators: modifiers (Ctrl/Control, Alt/Option, Shift, Super/Meta/Cmd/Win,
 * CommandOrControl) joined by '+' with one key: A-Z, 0-9, F1-F24, Space, Tab, Enter, Escape,
 * Backspace, Delete, Insert, Home, End, PageUp, PageDown, Up/Down/Left/Right, punctuation (','),
 * VolumeUp, VolumeDown, VolumeMute, MediaPlayPause, MediaNextTrack, MediaPreviousTrack,
 * MediaStop, PrintScreen. Modifiers alone ("Super") are a tap: pressed and released with no
 * other key or pointer button in between; it fires on the release.
 *
 * Delivery: under DRM (bro.window.displayMode === 'drm') every key is matched before it is
 * routed: a matched chord's key reaches neither the focused client nor the app's DOM (its
 * repeats and release included), and the callback runs before the next key is routed.
 * Modifier keys themselves are never swallowed. Other hosts accept the registration (Windows
 * also reserves the chord with RegisterHotKey) but do not deliver it yet, and headless only
 * through __bro_native.window.simulateGlobalHotkey; keep a keydown handler for those.
 *
 * @param {string} accelerator e.g. "Super+L", "CommandOrControl+Shift+T", "Alt+Tab", "Super"
 * @param {function({id:number, accelerator:string})} callback runs only for this registration
 * @param {Object} [options]
 * @param {boolean} [options.grab=false] When the chord fires, the app keeps the keyboard until
 *   every modifier of the chord has been released, the release included, so its DOM sees the
 *   keys that follow (arrows, Escape, the Alt keyup). For an Alt+Tab switcher: register
 *   'Alt+Tab' and 'Alt+Shift+Tab' with grab (each further Tab fires the callback again) and
 *   commit on the Alt keyup. DRM only.
 * @returns {number} hotkey ID (> 0) on success, 0 on failure
 */
bro.window.registerGlobalHotkey = function(accelerator, callback, options) {};

/**
 * Unregisters a previously registered global hotkey.
 * @param {number} id
 * @returns {boolean}
 */
bro.window.unregisterGlobalHotkey = function(id) {};

/**
 * Unregisters all global hotkeys.
 */
bro.window.unregisterAllGlobalHotkeys = function() {};

/**
 * Enforces single-instance application execution and forwards arguments from
 * subsequent launches.
 * @param {Object} options
 * @param {string} options.name Unique application identifier
 * @param {function(Array<string>)} [options.onInstance] Callback receiving forwarded argv
 * @returns {boolean} True if this is the primary instance, false if forwarded to existing instance
 */
bro.window.requestSingleInstance = function(options) {};

/**
 * Shuts down single-instance IPC listener and frees application lock.
 */
bro.window.shutdownSingleInstance = function() {};

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
 * hidden window) / getTitle() / setTitle(s) / getOpacity() / setOpacity(v) /
 * flash(on) / requestAttention(on) / focus() / close()
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

