/**
 * @file docs/decor-api.js
 * @summary Documentation and examples for the `bro.decor` JavaScript API.
 */

/**
 * `bro.decor` provides client-side window decoration geometry calculation,
 * caption button layout, shadow margins, and pointer hit-testing (backed by
 * native `brodecor`):
 * - Default frame metrics inspection (`getDefaultMetrics`)
 * - Frame, caption, title, icon, button, and shadow geometry calculation (`computeGeometry`)
 * - Point hit-testing against computed frame geometry for resize edges, caption drag, and window buttons (`hitTest`)
 *
 * Mounted automatically in Bronze when `BRO_WITH_DECOR` is enabled.
 */

// ============================================================================
// Type Definitions
// ============================================================================

/**
 * 2D rectangle in logical pixels.
 * @typedef {Object} DecorRect
 * @property {number} x
 * @property {number} y
 * @property {number} width
 * @property {number} height
 */

/**
 * 2D point in logical pixels.
 * @typedef {Object} DecorPoint
 * @property {number} x
 * @property {number} y
 */

/**
 * 4-sided margins in logical pixels.
 * @typedef {Object} DecorMargins
 * @property {number} left
 * @property {number} right
 * @property {number} top
 * @property {number} bottom
 */

/**
 * Default or custom frame decoration metrics.
 * @typedef {Object} DecorMetrics
 * @property {number} captionHeight - Height of the window title bar (default 32)
 * @property {number} borderThickness - Resize border thickness (default 4)
 * @property {number} resizeGripSize - Edge resize grip hit size (default 8)
 * @property {number} cornerRadius - Window frame corner radius (default 8)
 * @property {number} cornerGripLength - Corner resize grip length along edges (default 24)
 * @property {number} buttonWidth - Caption button width (default 36)
 * @property {number} buttonHeight - Caption button height (default 24)
 * @property {number} buttonSpacing - Spacing between caption buttons (default 2)
 * @property {number} shadowRadius - Drop shadow blur radius
 */

/**
 * Caption button layout entry inside `DecorGeometry.buttons`.
 * @typedef {Object} DecorButtonItem
 * @property {'Close'|'Minimize'|'Maximize'|'Restore'|'Menu'} kind - Button role
 * @property {DecorRect} rect - Button bounding box in frame coordinates
 */

/**
 * Options for `bro.decor.computeGeometry(options)`.
 * @typedef {Object} DecorGeometryOptions
 * @property {DecorRect} clientRect - Client area rectangle (width <= 0 defaults to 800, height <= 0 defaults to 600)
 * @property {{captionHeight?: number, borderThickness?: number, cornerRadius?: number}} [metrics] - Optional metric overrides
 * @property {'active'|'inactive'|'maximized'|'fullscreen'|'tiled'|'tiled_left'|'tiled_right'} [state='active'] - Window frame state
 * @property {string} [title] - Window title text
 */

/**
 * Computed window frame and decoration geometry returned by `bro.decor.computeGeometry`.
 * @typedef {Object} DecorGeometry
 * @property {DecorRect} frameRect - Outer window frame rectangle (client + borders + caption)
 * @property {DecorRect} clientRect - Inner client content rectangle
 * @property {DecorRect} captionRect - Title bar / caption rectangle
 * @property {DecorRect} titleRect - Title text bounding rectangle inside the caption
 * @property {DecorRect} iconRect - Window icon bounding rectangle inside the caption
 * @property {DecorButtonItem[]} buttons - Positioned caption buttons (Minimize, Maximize, Close)
 * @property {DecorMargins} shadowMargins - Drop shadow margins extending outside the frame
 */

/**
 * Hit-test result returned by `bro.decor.hitTest(geometry, point)`.
 * @typedef {Object} DecorHitTestResult
 * @property {'None'|'Client'|'Caption'|'Button'|'ResizeBorder'|'Shadow'} area - Hit region classification
 * @property {'None'|'Top'|'Bottom'|'Left'|'Right'|'TopLeft'|'TopRight'|'BottomLeft'|'BottomRight'} edge - Resize border edge or corner grip
 * @property {boolean} isClient - True when `area === 'Client'`
 * @property {boolean} isCaption - True when `area === 'Caption'`
 * @property {boolean} isButton - True when `area === 'Button'`
 * @property {boolean} isResizable - True when hitting a resize border with an active edge grip
 * @property {boolean} isDraggable - True when hitting the draggable caption area
 * @property {number} buttonIndex - Index into `geometry.buttons`, or `-1` if no button was hit
 * @property {'Close'|'Minimize'|'Maximize'|'Restore'|'Menu'|null} buttonKind - Kind of the hit caption button, or `null`
 */

// ============================================================================
// API Stubs
// ============================================================================

/**
 * Whether the `brodecor` decoration subsystem is compiled into this binary.
 * @type {boolean}
 */
bro.decor.available;

/**
 * Returns the default window decoration frame metrics.
 *
 * @returns {DecorMetrics}
 */
bro.decor.getDefaultMetrics = function() {};

/**
 * Computes the complete window frame, caption, button, and shadow geometry for
 * a given client rectangle, metric overrides, window state, and title.
 *
 * @param {DecorGeometryOptions} options
 * @returns {DecorGeometry|null} Computed geometry, or `null` if `options` is not an object
 */
bro.decor.computeGeometry = function(options) {};

/**
 * Hit-tests a 2D point against a computed `DecorGeometry` object to determine
 * whether the pointer is over the client area, draggable title bar, a caption
 * button, or a resize border/corner.
 *
 * @param {DecorGeometry} geometry - Geometry object returned by `computeGeometry`
 * @param {DecorPoint} point - Pointer coordinates `{ x, y }`
 * @returns {DecorHitTestResult|null} Hit-test classification, or `null` if arguments are invalid
 */
bro.decor.hitTest = function(geometry, point) {};

// ============================================================================
// Examples
// ============================================================================

if (bro.decor.available) {
    // 1. Inspect default decoration metrics
    const metrics = bro.decor.getDefaultMetrics();
    console.log('Caption height:', metrics.captionHeight, 'Corner radius:', metrics.cornerRadius);

    // 2. Compute frame geometry around a 1280x720 client area
    const geom = bro.decor.computeGeometry({
        clientRect: { x: 100, y: 80, width: 1280, height: 720 },
        metrics: { captionHeight: 34, borderThickness: 4, cornerRadius: 10 },
        state: 'active',
        title: 'Document Editor'
    });

    console.log('Outer frame:', geom.frameRect);
    console.log('Caption rect:', geom.captionRect);
    for (const btn of geom.buttons) {
        console.log(`Button ${btn.kind}:`, btn.rect);
    }

    // 3. Hit-test a pointer position against the frame
    const hit = bro.decor.hitTest(geom, { x: 250, y: 65 });
    if (hit.isButton) {
        console.log(`Clicked caption button: ${hit.buttonKind} (#${hit.buttonIndex})`);
    } else if (hit.isDraggable) {
        console.log('Begin window titlebar move');
    } else if (hit.isResizable) {
        console.log(`Begin window resize on edge: ${hit.edge}`);
    }
}
