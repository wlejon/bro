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
