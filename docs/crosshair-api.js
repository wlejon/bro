/**
 * bro.crosshair — Engine-level crosshair overlay
 *
 * Renders a configurable crosshair at the viewport center, drawn directly
 * in the GPU pipeline (GL color shader in windowed mode, Skia in CPU headless).
 * Runs at the full frame rate — independent of layout/raster cycles.
 *
 * The crosshair is drawn after all app content (HTML, Canvas 2D, WebGL, scene
 * graphs) but before system panels (perf, settings).
 */

// ── Show / Hide ──────────────────────────────────────────────────────────────

bro.crosshair.show();   // make the crosshair visible
bro.crosshair.hide();   // hide the crosshair

bro.crosshair.visible;  // read-only boolean — true if currently shown


// ── Configure ────────────────────────────────────────────────────────────────

/**
 * Configure the crosshair appearance. All properties are optional —
 * only the ones you pass are updated; others keep their current values.
 *
 * @param {Object} options
 * @param {string}  [options.style]             - 'cross' | 'dot' | 'circle' | 'crossdot'
 * @param {number}  [options.size]              - Arm length from center in pixels (default: 20)
 * @param {number}  [options.thickness]         - Line thickness in pixels (default: 2)
 * @param {number}  [options.gap]               - Gap around center in pixels (default: 4)
 * @param {number}  [options.dotSize]           - Center dot radius in pixels (default: 2)
 * @param {string}  [options.color]             - Color as '#RGB', '#RRGGBB', or '#RRGGBBAA' (default: '#00ff00')
 * @param {number}  [options.opacity]           - Opacity 0–1, overrides alpha channel (default: 0.8)
 * @param {boolean} [options.outline]           - Draw dark outline for visibility (default: true)
 * @param {number}  [options.outlineThickness]  - Outline width in pixels (default: 1)
 * @param {string}  [options.outlineColor]      - Outline color as hex string (default: '#000000b4')
 */
bro.crosshair.configure({
    style: 'cross',
    size: 20,
    thickness: 2,
    gap: 4,
    color: '#00ff00',
    opacity: 0.8,
    outline: true,
    outlineThickness: 1,
    outlineColor: '#000000b4'
});


// ── Styles ───────────────────────────────────────────────────────────────────

// 'cross' — Four arms extending from center with optional gap.
//           Most common FPS crosshair.
bro.crosshair.configure({ style: 'cross', size: 15, thickness: 2, gap: 4 });

// 'dot' — Single filled circle at center.
//         Minimal, precise aiming point.
bro.crosshair.configure({ style: 'dot', dotSize: 3 });

// 'circle' — Hollow circle (ring) around center.
//            Good for target-tracking games.
bro.crosshair.configure({ style: 'circle', size: 12, thickness: 2 });

// 'crossdot' — Cross arms + center dot combined.
//              Arms provide spatial reference, dot marks exact center.
bro.crosshair.configure({ style: 'crossdot', size: 20, gap: 6, dotSize: 2 });


// ── Typical FPS setup ────────────────────────────────────────────────────────

bro.crosshair.configure({
    style: 'crossdot',
    size: 12,
    thickness: 2,
    gap: 3,
    dotSize: 1,
    color: '#00ff00',
    opacity: 1.0,
    outline: true,
    outlineThickness: 1,
    outlineColor: '#000000'
});
bro.crosshair.show();


// ── Headless testing ─────────────────────────────────────────────────────────

// Use getPixel(x, y) to validate crosshair rendering:
//
//   bro.crosshair.configure({ style: 'cross', size: 10, gap: 0, color: '#00ff00',
//                              opacity: 1.0, outline: false, thickness: 2 });
//   bro.crosshair.show();
//   flush();
//   var p = getPixel(960, 540);  // center of 1920x1080 viewport
//   assert(p.g > 200, 'crosshair center should be green');
