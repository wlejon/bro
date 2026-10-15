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
 * Whether the window composites with per-pixel alpha over the desktop: where
 * the page paints nothing (a transparent `html` and `body` background) what is
 * behind the window shows through, and translucent CSS (rgba, `opacity`,
 * shadows) blends with it, premultiplied end to end. Declared in bro.json
 * (`"transparent": true`, docs/apps.md) and fixed when the window is created;
 * this reads whether it took. False for a secondary window (bro.window.open).
 *
 * - Windows: the Vulkan swapchain presents with premultiplied composite alpha
 *   to a window SDL sets up for DWM's alpha (blur-behind with an empty blur
 *   region). NVIDIA's driver offers it (measured: no change in present time
 *   against an opaque window); a driver that offers only opaque composite
 *   alpha leaves the window opaque, and this reads false.
 * - Linux, Wayland (bro's own client): an ARGB buffer (premultiplied
 *   composite alpha) and no opaque region. X11 (through SDL): an ARGB visual.
 * - macOS: a non-opaque NSWindow over a non-opaque CAMetalLayer.
 * - Headless: captures (`screenshot()`, `frameImage()`, `getPixel()`) carry
 *   the real alpha, unpremultiplied as PNG and ImageData are; an ordinary
 *   window's captures are opaque.
 * @readonly
 * @type {boolean}
 */
bro.window.transparent;

/**
 * Click-through: pointer input over a point where the page would hit nothing
 * goes to whatever lies beneath the window, other applications' windows
 * included; over an element the window takes it as usual. "Nothing" is the
 * page's own hit test landing on `<html>` or `<body>` — no element there, or
 * `pointer-events: none` all the way down — so CSS alone decides what is
 * clickable. The menu bar, an open dropdown and a system panel take input.
 * It follows the page as it changes (hover, animations, elements appearing):
 * after each frame that laid out or painted, never while the page is still.
 * Declared in bro.json (`"clickThrough": true`) or set here at run time;
 * setting it where the window system cannot pass input through a window
 * leaves it false. Main window only. Meant for a `transparent` window, though
 * on Windows it works on an opaque one too.
 *
 * - Windows: a top-level window passes input to other processes only as
 *   WS_EX_LAYERED | WS_EX_TRANSPARENT, and then all of it, so the window
 *   follows the pointer: a low-level mouse hook (its own thread) notes where
 *   the pointer is over the window, and the main thread asks the page's hit
 *   test about that one point and flips WS_EX_TRANSPARENT when the answer
 *   changes. Measured: ~17 µs of main-thread work per pointer move over the
 *   window, ~0.6 ms for a flip (only when the pointer crosses an element's
 *   edge), nothing while the pointer and the page are still. A press the
 *   window took keeps it taking input until the release (drags).
 * - Wayland: `wl_surface.set_input_region` from the rectangles where the hit
 *   test takes input. X11: the same rectangles as the XShape input shape
 *   (needs `transparent`). macOS: `ignoresMouseEvents` from the same
 *   rectangles (needs `transparent`). Those rectangles cost a hit test per
 *   cell of the page's box grid: well under a millisecond for an overlay;
 *   a page whose region costs more than 2 ms has it redone at most every
 *   eighth of the time while it keeps changing, and caught up once it stops.
 * - Headless: `takesPointerAt(x, y)` and `inputRegion()` answer the same
 *   questions (docs/headless.md).
 * @type {boolean}
 *
 * @example
 *   // bro.json: { "transparent": true, "clickThrough": true,
 *   //             "borderless": true, "alwaysOnTop": true }
 *   document.documentElement.style.background = 'transparent';
 *   document.body.style.background = 'transparent';
 *   const note = document.createElement('div');   // drawn, never in the way
 *   note.style.cssText = 'position:absolute;left:20px;top:20px;width:200px;height:80px;' +
 *       'background:rgba(255,220,0,.35);border-radius:10px;pointer-events:none';
 *   const close = document.createElement('button'); // takes clicks
 *   close.textContent = 'Close';
 *   close.style.cssText = 'position:absolute;right:12px;bottom:12px';
 *   close.onclick = () => bro.quit();
 *   document.body.append(note, close);
 *   // While the user edits the note, the whole window takes input:
 *   function editing(on) { bro.window.clickThrough = !on; }
 */
bro.window.clickThrough;

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
 * @param {Array<{action: string, title?: string}>} [options.actions]
 * @param {string} [options.payload]
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
 * How registered global hotkeys reach the app (registerGlobalHotkey below): 'keyboard' when
 * the host sees every key first and taps, grabs and every chord work (DRM; the Windows desktop
 * shell's keyboard hook), 'system' when chords are only reserved with the OS (an ordinary
 * Windows app), 'none' otherwise (headless: simulate them). A shell registers its whole table
 * under 'keyboard', and only chords the desktop around it will not take otherwise.
 * @type {'keyboard'|'system'|'none'}
 * @readonly
 */
bro.window.hotkeyRouting;

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
 * Modifier keys themselves are never swallowed. The Windows desktop shell (helm as the
 * Winlogon shell; EngineConfig::desktopShell) is routed the same way, by a low-level keyboard
 * hook that runs only while a hotkey is registered: a lone Super tap fires its binding, a
 * registered chord's key reaches neither the system nor the focused application (Win+V and
 * the other chords Windows keeps for itself included), unregistered Win combinations pass
 * untouched, and a Win or Alt release after a swallowed key is not taken for a tap (no Start
 * menu, no menu bar). Win+L cannot be had: the secure desktop takes it before any hook.
 * `bro.window.hotkeyRouting` says which delivery a host gives: 'keyboard' for both of these.
 * Other hosts accept the registration (an ordinary Windows app also reserves the chord with
 * RegisterHotKey; no hook) but do not deliver it yet ('system'), and headless only through
 * __bro_native.window.simulateGlobalHotkey ('none'); keep a keydown handler for those.
 *
 * @param {string} accelerator e.g. "Super+L", "CommandOrControl+Shift+T", "Alt+Tab", "Super"
 * @param {function({id:number, accelerator:string})} callback runs only for this registration
 * @param {Object} [options]
 * @param {boolean} [options.grab=false] When the chord fires, the app keeps the keyboard until
 *   every modifier of the chord has been released, the release included, so its DOM sees the
 *   keys that follow (arrows, Escape, the Alt keyup). For an Alt+Tab switcher: register
 *   'Alt+Tab' and 'Alt+Shift+Tab' with grab (each further Tab fires the callback again) and
 *   commit on the Alt keyup. DRM and the Windows desktop shell (there, while another
 *   application has the foreground, the held keys are swallowed and delivered to the
 *   document instead).
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
 * @property {boolean} focused  whether this secondary window currently has focus
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

