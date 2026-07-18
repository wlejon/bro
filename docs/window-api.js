/**
 * bro.window — runtime window management, plus the web window/system
 * surfaces that ride along with it (window.screen, window.open,
 * navigator.getBattery).
 *
 * ONE home for window state: startup configuration comes from bro.json
 * manifest keys (parsed once at engine construction), runtime control is
 * bro.window.*. Window SIZE and FULLSCREEN intentionally stay where they
 * already live — bro.settings graphics.width / graphics.height /
 * graphics.fullscreen (persisted, user-overridable) — bro.window covers the
 * imperative state that is not a persisted preference: borderless,
 * always-on-top, resize limits, position, minimize/maximize/restore, and
 * display placement.
 *
 * Transparent (per-pixel-alpha) windows are deliberately NOT offered: the
 * compositor owns the GL swap chain, and a transparent window needs a
 * different swap-chain/visual setup on every platform — deferred until
 * something needs it.
 *
 * Headless: the same API installs against the hidden window, pinned for
 * determinism. Flag/limit setters (borderless, alwaysOnTop, setMinSize,
 * setMaxSize) still apply and round-trip — they are pure window state.
 * State-affecting ops (minimize/maximize/restore, setPosition,
 * moveToDisplay) no-op (moveToDisplay returns false) so a test can never
 * disturb the window the whole pipeline renders through. getDisplays()
 * enumerates the real displays of the machine (assert shapes, not values,
 * in tests). Under --no-gpu there is no window at all: queries return their
 * defaults ('normal', 0/0, empty display list) and every mutator no-ops.
 */

// ── bro.json manifest keys (startup) ─────────────────────────────────────────
//
// {
//     "borderless": true,        // no title bar / border (SDL_WINDOW_BORDERLESS)
//     "alwaysOnTop": true,       // keep above all normal windows
//     "minWidth": 320,           // resize limits, px; omit = unconstrained
//     "minHeight": 240,
//     "maxWidth": 1600,
//     "maxHeight": 900,
//     "windowX": 100,            // explicit startup position (BOTH must be set;
//     "windowY": 100,            //  negative values legal on multi-monitor)
//     "display": 1               // display INDEX to center on at startup
// }
//
// windowX/windowY win over "display" when both are given. Positioning keys
// are skipped in headless mode (hidden window).

// ── Properties ───────────────────────────────────────────────────────────────

bro.window.state;         // string, read-only — 'normal' | 'minimized' |
                          // 'maximized' | 'fullscreen' (fullscreen wins over
                          // maximized; minimized wins over both)

bro.window.borderless;        // boolean — window border + title bar removed
bro.window.borderless = true;

bro.window.alwaysOnTop;       // boolean — kept above all normal windows
bro.window.alwaysOnTop = true;

// ── Window state control ─────────────────────────────────────────────────────

bro.window.minimize();    // → state 'minimized'; document.hidden flips true
                          //   and 'visibilitychange' fires (web semantics —
                          //   same event focus loss already fires)
bro.window.maximize();    // → state 'maximized' (a normal RESIZED event
                          //   follows; layout adapts as with any resize)
bro.window.restore();     // un-minimize / un-maximize → 'normal';
                          //   document.hidden flips back false

// ── Position + resize limits ─────────────────────────────────────────────────

bro.window.getPosition(); // → { x, y } — desktop coordinates of the window
bro.window.setPosition(100, 200);

bro.window.getMinSize();  // → { width, height } — 0,0 = unconstrained
bro.window.setMinSize(320, 240);
bro.window.setMinSize(0, 0);   // clear

bro.window.getMaxSize();  // → { width, height } — 0,0 = unconstrained
bro.window.setMaxSize(1600, 900);
bro.window.setMaxSize(0, 0);   // clear

// ── Displays ─────────────────────────────────────────────────────────────────

bro.window.getDisplays();
// → [{
//     id: 1,                    // SDL display id (stable while attached) —
//                               // pass to moveToDisplay
//     name: 'DELL U2723QE',
//     bounds:   { x, y, width, height },  // full display, desktop coords
//     workArea: { x, y, width, height },  // minus taskbar/dock
//     refreshRate: 144,         // desktop mode, Hz
//     contentScale: 1.5,        // OS scaling (1.5 = 150%)
//     isPrimary: true,
//     isCurrent: true           // the display this window sits on
//   }, ...]
// Fullscreen display MODES (resolutions for exclusive fullscreen) remain on
// bro.settings.getDisplayModes() — current display only.

bro.window.moveToDisplay(id);   // center the window on that display's work
                                // area → boolean (false: unknown id, or
                                // headless). Explicit coords: setPosition.

// ── window.screen (all realms, incl. iframes) ────────────────────────────────
//
// Live accessors over the display the window CURRENTLY sits on — values
// follow the window across monitors. Headless pins all four to the hidden
// window's size (same determinism policy as devicePixelRatio).

screen.width;        // full display width, px
screen.height;
screen.availWidth;   // work area (minus taskbar/dock)
screen.availHeight;
screen.colorDepth;   // always 24
screen.pixelDepth;   // always 24

// ── window.open(url) — shell URL handoff ─────────────────────────────────────
//
// Hands the URL to the OS handler (default browser, mail client) via
// SDL_OpenURL. ALWAYS returns null — there is no popup Window object to
// return; target/features arguments are accepted and ignored.
//
// Trust model: any scheme the OS accepts is allowed — http(s), mailto, and
// yes, file:// and friends. bro apps are local-first: they already run with
// full fs + child_process access, so restricting URL schemes here would
// protect nothing. Treat window.open like any other local capability.
// Headless never shells out (logs and returns null).

window.open('https://example.com');       // → null (browser opens)
window.open('mailto:someone@example.com'); // → null (mail client opens)

// ── navigator.getBattery() ───────────────────────────────────────────────────
//
// Promise of a BatteryManager-shaped SNAPSHOT over SDL_GetPowerInfo.
// Snapshot-on-call: values are captured when you call it — call again for
// fresh values. There are NO change events (the addEventListener /
// on*change members exist but never fire); poll if you need updates.
//
// Mapping: no battery / unknown (desktops) → web convention
//   { charging: true, chargingTime: 0, dischargingTime: Infinity, level: 1 }
// On battery → charging false, dischargingTime = seconds left (Infinity if
// unknown), level 0..1. Charging → charging true, chargingTime Infinity
// (SDL has no time-to-full estimate). Headless always reports the
// no-battery shape.

const battery = await navigator.getBattery();
battery.charging;         // boolean
battery.chargingTime;     // seconds; 0 = full or no battery
battery.dischargingTime;  // seconds until empty; Infinity on AC
battery.level;            // 0.0 .. 1.0

// ── bro.window.open(src, opts) — secondary OS windows ────────────────────────
//
// *** v1 IN PROGRESS ***  A secondary window is a full, isolated bro app in
// its own OS window: `src` (an app directory or index.html, resolved exactly
// like <iframe src>) is loaded into its own JS realm, DOM tree, timers and 2D
// canvas scenes, laid out at the window's client size and rendered into it.
// The handle controls geometry/title/focus, reports 'load' when the document
// is ready, exposes capture() for the window's pixels, and fires 'close' on
// close() / the OS close button.
//
// Still to come (next chunk of the multiwindow plan): postMessage in both
// directions, 'message' events, window.close() self-close, and INPUT — mouse,
// keyboard and IME still go to the main window only, so a secondary window
// renders and animates but does not yet respond to clicks or typing.
//
// v1 refusals inside a secondary window's document, each logged as a clear
// warning rather than failing silently: WebGL contexts (getContext('webgl')
// returns null), 3D scene graphs (bro.scene is not installed in a child
// realm, as in iframes), and nested <iframe> elements (the element lays out
// as an empty box, its src never loads).
//
// The child realm gets the standard sub-document bindings — DOM, timers,
// 2D canvas, storage, settings, images — plus a bro.window scoped to ITS
// window: bro.window.state / getPosition / setPosition / minimize / maximize
// / restore / borderless / alwaysOnTop / getDisplays all act on the secondary
// window, and window.screen / devicePixelRatio report for the display it sits
// on. bro.window.open itself stays main-realm-only and throws there.
//
// Realm policy: only the MAIN app realm may open windows. Calling
// bro.window.open from an iframe (or any other child realm) throws
// "bro.window.open is only available from the main app realm".
//
// Lifecycle is asynchronous: open() returns the handle immediately, but the
// OS window materializes at the engine's next idle drain (same cadence as
// iframe reloads). Geometry getters answer with the requested values until
// then. close() likewise queues: `closed` flips (and 'close' fires, once)
// at the drain. Double-close is a no-op. Closing the MAIN window quits the
// whole app, secondary windows included.
//
// Headless: secondary windows are always hidden and their display scale is
// pinned with the rest of the pipeline. setSize round-trips (pure window
// state); setPosition no-ops on hidden windows (desk-dependent); focus()
// no-ops. flush() runs the drain, so open/close are assertable:
//   const w = bro.window.open('palette', { width: 320 });  flush();
// flush() also runs the child's timers under virtual time, and capture()
// works, so a headless test can drive and assert a secondary window end to end.

const win = bro.window.open('palette', {   // app dir or index.html, like <iframe src>
    width: 320, height: 200,               // client size, px (default 800x600)
    title: 'Palette',                      // window title (default 'bro')
    x: 100, y: 80,                         // desktop position (BOTH or neither)
    display: 1,                            // display INDEX to center on (like bro.json)
    resizable: true,                       // default true
    borderless: false,                     // no title bar / border
    alwaysOnTop: false,                    // keep above normal windows
    hidden: false,                         // create hidden (headless forces true)
});

win.id;                  // number, read-only — stable handle id
win.closed;              // boolean, read-only — true once the window is gone
win.close();             // queue destroy → 'close' fires at the drain; idempotent
win.setTitle('Palette'); // retitle
win.getSize();           // → { width, height }
win.setSize(400, 300);   // resize (applies to hidden windows too)
win.getPosition();       // → { x, y } desktop coords
win.setPosition(60, 40); // move (no-op on hidden windows)
win.focus();             // raise + request input focus (no-op hidden)
win.addEventListener('close', (ev) => {  // OS close button or close();
    ev.target === win;                   // fires exactly once, after which
    win.closed === true;                 // closed already reads true
});
win.removeEventListener('close', fn);

win.addEventListener('load', (ev) => {   // the window's document is parsed,
    ev.target === win;                   // scripted and laid out — fires once,
});                                      // at the drain after open()

// win.capture() — the window's pixels as ImageData ({ width, height, data },
// top-down RGBA), or null before the document loads / after close. The engine
// re-records the document at its current size on the calling thread, so a
// capture taken right after a change shows that change (no rAF timing games).
// This is also how a headless test observes a secondary window at all.
const shot = win.capture();
shot.width; shot.height; shot.data;      // Uint8ClampedArray, 4 bytes per px

// Resizing the window (setSize, or the user dragging its edge) updates the
// child realm's innerWidth/innerHeight and fires a 'resize' event there, then
// re-lays-out and re-renders the document at the new size.

// NEXT CHUNK (not yet available): win.postMessage(data, transfer), 'message'
// events, window.close() self-close, and input routing into the window.
