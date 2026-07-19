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
 * defaults ('normal', 0/0, empty display list) and every mutator no-ops —
 * except bro.window.open(), which THROWS "bro.window.open requires a GPU
 * window session (unavailable under --no-gpu)": a secondary window needs a
 * primary one to share a swap chain with, and silently returning a dead
 * handle would be worse than the error.
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
// On battery → charging false, chargingTime Infinity (not charging, so there
// is no time-to-full), dischargingTime = seconds left (Infinity if unknown),
// level 0..1. Charging → charging true, chargingTime Infinity (SDL has no
// time-to-full estimate). Charged (on AC, battery full) → charging true,
// chargingTime 0 — the one case where chargingTime is finite. Headless always
// reports the no-battery shape.

const battery = await navigator.getBattery();
battery.charging;         // boolean
battery.chargingTime;     // seconds until full; 0 = already full or no
                          // battery, Infinity whenever it is unknown
battery.dischargingTime;  // seconds until empty; Infinity on AC
battery.level;            // 0.0 .. 1.0

// ── bro.window.open(src, opts) — secondary OS windows ────────────────────────
//
// A secondary window is a full, isolated bro app in its own OS window: `src`
// (an app directory or index.html, resolved exactly like <iframe src>) is
// loaded into its own JS realm, DOM tree, timers and 2D canvas scenes, laid
// out at the window's client size and rendered into it. The handle controls
// geometry/title/focus, carries messages to and from the child realm, reports
// 'load' when the document is ready, exposes capture() for the window's
// pixels, and fires 'close' on close() / the OS close button.
//
// Input is routed per window. A secondary window handles its own mouse
// (click / dblclick / hover / :hover / enter-leave / drag-select), CSS cursor,
// keyboard, Tab focus, text input and IME, wheel scrolling of its overflow
// boxes, and file/text drops — all against ITS document, with its own focus,
// hover target and click streak. Nothing crosses over: a click in a palette
// window can never focus an element in the main app, and vice versa. Focus,
// page visibility (document.hidden + visibilitychange) and window resize are
// likewise per realm.
//
// ── NOT SUPPORTED IN SECONDARY WINDOWS (v1) ─────────────────────────────────
//
// The complete list, each with the reason. Nothing here fails silently: a
// refusal logs a clear warning, returns null, or throws.
//
//   • WebGL contexts — getContext('webgl'/'webgl2') returns null. The GL
//     frame path assumes the app document owns the single WebGL entry list.
//   • 3D scene graphs — bro.scene is not installed in a child realm at all
//     (same as an <iframe>); the 3D renderer draws into the main window.
//   • Nested <iframe> — the element lays out as an empty box and its src never
//     loads: the iframe sync walks only the app document.
//   • bro.window.open from a child realm — throws "only available from the
//     main app realm"; windows are opened by the app, never by its windows.
//   • System panels, the menu bar and the inspector — main-window chrome; a
//     secondary window carries no engine furniture.
//   • Pointer lock — element.requestPointerLock() in a secondary window's
//     realm (or an iframe's) throws: SDL's relative mouse mode is bound to the
//     primary window, so a lock here would capture the wrong pointer.
//   • Touch — finger events on a secondary window are dropped rather than
//     misrouted: the engine's contact table is global, with no per-window key.
//   • Gamepad and bro.settings "action" events — always delivered to the main
//     app realm regardless of which window has focus. Keystrokes in a
//     secondary window fire DOM key events there but never an "action", so
//     typing a "w" into a palette's text field can't trigger a movement
//     binding.
//   • Overlays — a <select> or <input type="color"> focuses and takes keys,
//     but its dropdown / colour-picker popup does not open: the overlay
//     manager draws on the main window.
//   • contenteditable editing (and its IME composition) — a contenteditable
//     element receives key/text DOM events, but the engine does not splice
//     text into it. <input> and <textarea> have full support, IME included.
//   • Viewport scrolling — no engine viewport scrollbar; a document that
//     overflows scrolls only through its own overflow: auto/scroll boxes.
//   • Layout-thread layout — a host document lays out synchronously on the
//     main thread (the layout thread is hard-bound to the app document).
//   • True HiDPI surfaces — host surfaces are rendered at window-size units
//     and scaled to the drawable, so a 2x display is not pixel-crisp yet.
//
// Deliberately never coming: moving DOM nodes between windows. Realms are
// isolated by construction; postMessage is the contract.
//
// System hotkeys stay GLOBAL: the perf-HUD and settings-modal bindings fire
// whichever bro window has focus (the panels themselves render on the main
// window).
//
// The child realm gets the standard sub-document bindings — DOM, timers,
// 2D canvas, storage, settings, images — plus a bro.window scoped to ITS
// window: bro.window.state / getPosition / setPosition / getMinSize /
// setMinSize / getMaxSize / setMaxSize / minimize / maximize / restore /
// borderless / alwaysOnTop / getDisplays / moveToDisplay all act on the
// secondary window, and window.screen / devicePixelRatio report for the
// display it sits on. bro.window.open itself stays main-realm-only and
// throws there.
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
// flush() drains creates, closes, resizes and messages only — the child's
// timers and rAF run from advanceTime()'s stepping loop, so a test that waits
// on a setTimeout inside the window app must advanceTime(), not flush(). With
// those two plus capture(), a headless test can drive and assert a secondary
// window end to end.

const win = bro.window.open('palette', {   // app dir or index.html, like <iframe src>
    width: 320, height: 200,               // client size, px (default 800x600)
    title: 'Palette',                      // window title (default 'bro')
    x: 100, y: 80,                         // desktop position (BOTH or neither)
    display: 1,                            // display INDEX to center on (like bro.json)
    resizable: true,                       // default true
    borderless: false,                     // no title bar / border
    alwaysOnTop: false,                    // keep above normal windows
    hidden: false,                         // create hidden (headless forces true)
    minWidth: 200, minHeight: 150,         // resize limits, 0 = unconstrained
    maxWidth: 0,   maxHeight: 0,
});

// ── The child app's own bro.json supplies the rest ───────────────────────────
//
// A window app is a normal bro app, so it may declare its own window shape.
// These manifest keys are honoured when the window opens: title, width,
// height, resizable, borderless, alwaysOnTop, minWidth/minHeight/maxWidth/
// maxHeight. Precedence, highest first:
//
//     explicit bro.window.open() options  >  the child's bro.json  >  defaults
//
// So a palette app that ships {"title":"Palette","width":240,"height":300,
// "alwaysOnTop":true} opens exactly that way from a bare
// bro.window.open('palette'), and the opener can still override any single key.
// windowX/windowY/display in a child bro.json are ignored — placement of a
// window the app opened belongs to the opener (x/y/display options).
//
// The child app is loaded BEFORE its OS window is created, so a src that does
// not resolve never flashes an empty window: the handle just closes.

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

win.addEventListener('resize', (ev) => { // the window's client size changed
    ev.width; ev.height;                 // (setSize, or the user dragging its
});                                      // edge). The child realm gets its own
                                         // window 'resize' event independently.

// ── Failure modes ────────────────────────────────────────────────────────────
//
// Bad input is either a throw or a documented no-op — never a half-opened
// window:
//
//   • open(src) with a non-string or empty src throws
//     "bro.window.open(src, opts): src must be a string" / "must be non-empty".
//   • open({ display: N }) with N past the attached display count logs
//     "bro.window.open: display=N, but only K display(s) attached" and falls
//     back to default placement — the window still opens.
//   • setSize(w, h) with w < 1 or h < 1 is silently ignored (the previous size
//     stands); open()'s width/height options clamp up to 1 instead.
//   • addEventListener drops what it cannot deliver, DOM-style: an unknown
//     event type, or a second argument that is not a function, registers
//     nothing and reports nothing.
//
// ── Messaging ────────────────────────────────────────────────────────────────
//
// Both directions are structured clones — the same encoder Worker.postMessage
// uses, so objects, arrays, Maps/Sets, typed arrays, ArrayBuffers, Mesh and
// ImageBitmap all cross, and functions throw "not cloneable". The optional
// second argument is a transfer list; transferred ArrayBuffers are DETACHED on
// the sending side, exactly as on the web.
//
// A Mesh must be TRANSFERRED, never cloned: one not listed in the transfer
// list throws "postMessage: Mesh must be listed in the transferList" (and one
// already transferred throws "Mesh is already neutered"). ImageBitmap is
// happy either way — unlisted it ref-shares its immutable image rather than
// copying pixels.
//
// Delivery is asynchronous and never reentrant: messages queue and are
// delivered at the engine's idle point (the same drain that materializes and
// destroys windows), so a 'message' handler never runs mid-frame. Children are
// delivered first and the parent second, which means a reply posted from a
// child's handler completes the round trip within the SAME drain — in headless,
// one flush().
//
// Posting to a window that has closed is a silent no-op (the clone still
// happens, so transfers still detach), never an error and never a crash.

win.postMessage({ type: 'current', color: '#3b82f6' });      // parent → child
win.postMessage({ buf: pixels.buffer }, [pixels.buffer]);    // …with a transfer

win.addEventListener('message', (ev) => {   // child → parent
    ev.data;                                // the cloned payload
    ev.target === win;
});

// Inside the child window's realm:
//
//   window.addEventListener('message', (ev) => { ev.data; });  // from parent
//   bro.window.parent.postMessage({ type: 'color', color: c }); // to parent
//   window.onmessage = (ev) => { ... };                        // also works
//
// window.close() inside a secondary window's realm closes THAT window: full
// teardown, and the parent handle's 'close' fires. (In the main app realm
// window.close() keeps its meaning — quit the app.)

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

// Headless input: every injection seam (click / mouseDown / mouseUp /
// mouseMove / keyDown / keyUp / textInput / imeCompose / imeCommit /
// imeCancel / wheel / dropText / dropFiles / currentCursor) takes an optional
// trailing windowId. Omitted means the main window; pass win.id to drive a
// secondary one. Coordinates there are plain window coordinates (no menu-bar
// inset, no viewport scroll). See docs/headless.md.
//
//   click(50, 30, 0, win.id);          // click inside the palette window
//   textInput('hi', win.id);           // type into its focused control
//   currentCursor(win.id);             // its own resolved cursor shape
//
// ── Perf HUD ─────────────────────────────────────────────────────────────────
//
// The perf panel (F8) grows a "Secondary windows" section listing each live
// host — title, client size, and whether it is focused or minimized. Each row
// is one more document recorded, replayed and composited every frame.
//
// ── Sample app ───────────────────────────────────────────────────────────────
//
// tests/manual/multiwindow_demo — a main window plus a tool-palette window
// exchanging messages (the palette posts a colour, the main window applies it
// and posts its current colour back; the palette's Close button self-closes).
// The palette's own bro.json supplies its size, title and alwaysOnTop.
//
//   ./build/Release/bro.exe tests/manual/multiwindow_demo
