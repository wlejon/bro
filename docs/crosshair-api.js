/**
 * bro.crosshair — Engine-level crosshair overlay
 *
 * Renders a configurable crosshair at the viewport center, drawn directly
 * in the GPU pipeline (GL color shader in windowed mode, Skia in CPU headless).
 * Runs at the full frame rate — independent of layout/raster cycles.
 *
 * The crosshair is drawn after all app content (HTML, Canvas 2D, WebGL, scene
 * graphs) but before system panels (perf, settings).
 *
 * Includes a built-in spread system: the gap between crosshair arms represents
 * bullet spread. The engine handles smooth interpolation, fire bloom decay, and
 * movement/ADS state — apps just set flags and the crosshair animates.
 */

// ── Show / Hide ──────────────────────────────────────────────────────────────

bro.crosshair.show();   // make the crosshair visible
bro.crosshair.hide();   // hide the crosshair

bro.crosshair.visible;  // read-only boolean — true if currently shown


// ── Configure ────────────────────────────────────────────────────────────────

/**
 * Configure the crosshair appearance and spread behavior. All properties are
 * optional — only the ones you pass are updated; others keep their current values.
 *
 * @param {Object} options
 *
 * Visual:
 * @param {string}  [options.style]             - 'cross' | 'dot' | 'circle' | 'crossdot'
 * @param {number}  [options.size]              - Arm length from center in pixels (default: 20)
 * @param {number}  [options.thickness]         - Line thickness in pixels (default: 2)
 * @param {number}  [options.dotSize]           - Center dot radius in pixels (default: 2)
 * @param {string}  [options.color]             - Color as '#RGB', '#RRGGBB', or '#RRGGBBAA' (default: '#00ff00')
 * @param {number}  [options.opacity]           - Opacity 0–1, overrides alpha channel (default: 0.8)
 * @param {boolean} [options.outline]           - Draw dark outline for visibility (default: true)
 * @param {number}  [options.outlineThickness]  - Outline width in pixels (default: 1)
 * @param {string}  [options.outlineColor]      - Outline color as hex string (default: '#000000b4')
 *
 * Spread system:
 * @param {number}  [options.spread]            - Base spread / idle gap in pixels (default: 4)
 * @param {number}  [options.gap]               - Alias for spread (backward compat)
 * @param {number}  [options.moveSpread]        - Extra spread when moving (default: 0)
 * @param {number}  [options.fireBloom]         - Spread kick per shot (default: 0)
 * @param {number}  [options.adsSpread]         - Spread when aiming down sights; -1 = no ADS (default: -1)
 * @param {number}  [options.bloomDecay]        - Bloom recovery speed in pixels/sec (default: 40)
 * @param {number}  [options.lerpSpeed]         - Spread interpolation speed; higher = faster (default: 10)
 */
bro.crosshair.configure({
    style: 'cross',
    size: 20,
    thickness: 2,
    color: '#00ff00',
    opacity: 0.8,
    outline: true,
    outlineThickness: 1,
    outlineColor: '#000000b4',
    // Spread
    spread: 4,
    moveSpread: 8,
    fireBloom: 6,
    adsSpread: 1,
    bloomDecay: 40,
    lerpSpeed: 10,
});


// ── Spread System ────────────────────────────────────────────────────────────
//
// The gap between crosshair arms shows the player where bullets will land.
// The engine computes: target = (aiming ? adsSpread : spread) + (moving ? moveSpread : 0) + bloom
// and smoothly interpolates currentSpread toward that target each frame.
//
// Bloom decays automatically at bloomDecay pixels/sec.
// Apps just set state flags — the engine handles all animation.

/** Tell the engine the player is moving (adds moveSpread to gap). */
bro.crosshair.setMoving(true);
bro.crosshair.setMoving(false);

/** Tell the engine the player is aiming down sights (tightens to adsSpread). */
bro.crosshair.setAds(true);
bro.crosshair.setAds(false);

/**
 * Add fire bloom — kicks the spread out, then it decays automatically.
 * With no argument, uses the configured fireBloom value.
 * With an argument, adds that exact amount (useful for per-frame continuous fire).
 */
bro.crosshair.addBloom();      // add configured fireBloom amount
bro.crosshair.addBloom(12);    // add custom amount

/**
 * Read the current effective spread (pixels). Use this for bullet spread
 * calculations — it reflects the exact gap the player sees.
 */
let accuracy = bro.crosshair.currentSpread;


// ── Manual Override ──────────────────────────────────────────────────────────
//
// For apps that want full control over spread each frame.

/** Set an exact spread value, bypassing the automatic spread system. */
bro.crosshair.setSpread(15.5);

/** Re-enable automatic spread (stops manual override). */
bro.crosshair.autoSpread();


// ── Styles ───────────────────────────────────────────────────────────────────

// 'cross' — Four arms extending from center with gap = spread.
bro.crosshair.configure({ style: 'cross', size: 15, thickness: 2, spread: 4 });

// 'dot' — Single filled circle at center.
bro.crosshair.configure({ style: 'dot', dotSize: 3 });

// 'circle' — Hollow circle (ring) around center.
bro.crosshair.configure({ style: 'circle', size: 12, thickness: 2 });

// 'crossdot' — Cross arms + center dot combined.
bro.crosshair.configure({ style: 'crossdot', size: 20, spread: 6, dotSize: 2 });


// ── Typical FPS setup ────────────────────────────────────────────────────────

bro.crosshair.configure({
    style: 'crossdot',
    size: 12,
    thickness: 2,
    spread: 3,
    dotSize: 1,
    color: '#00ff00',
    opacity: 1.0,
    outline: true,
    outlineThickness: 1,
    outlineColor: '#000000',
    moveSpread: 8,
    fireBloom: 6,
    adsSpread: 1,
    bloomDecay: 30,
    lerpSpeed: 10,
});
bro.crosshair.show();

// In game loop:
// bro.crosshair.setMoving(isPlayerMoving);
// bro.crosshair.setAds(isAiming);
// On each shot: bro.crosshair.addBloom();
// For bullet calc: let spread = bro.crosshair.currentSpread;


// ── Simple static crosshair (no spread) ──────────────────────────────────────

// For non-FPS apps, just set spread: 0 and don't call any spread methods.
bro.crosshair.configure({ style: 'circle', size: 6, thickness: 1, spread: 0,
                           color: '#ffffff', opacity: 0.7, outline: false });
bro.crosshair.show();


// ── Headless testing ─────────────────────────────────────────────────────────

// Use getPixel(x, y) to validate crosshair rendering:
//
//   bro.crosshair.configure({ style: 'cross', size: 10, spread: 0, color: '#00ff00',
//                              opacity: 1.0, outline: false, thickness: 2 });
//   bro.crosshair.show();
//   flush();
//   var p = getPixel(960, 540);  // center of 1920x1080 viewport
//   assert(p.g > 200, 'crosshair center should be green');
//
// For spread system tests, use advanceTime() to let interpolation run:
//
//   bro.crosshair.setMoving(true);
//   advanceTime(500);
//   assert(bro.crosshair.currentSpread > 10, 'spread increased while moving');
