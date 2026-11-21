/**
 * =============================================================================
 * bro.eye — deterministic visual, DOM, and 3D spatial measurement & critic
 * =============================================================================
 *
 * The visual twin of bro.ear: turn rendered frames, DOM layouts, and 3D
 * Vulkan depth/scene snapshots into deterministic perceptual numbers, defect
 * lists, annotated diagnostic pictures, and closed-loop parameter fits —
 * without relying on opaque VLM guesses or neural weights.
 *
 *   bro.eye.measure(frame?, opts?)            Oklab luminance/zones, banding,
 *                                             chroma/hue entropy, dominant palette,
 *                                             sharpness, clutter, whitespace,
 *                                             2D FFT tiling & balance
 *   bro.eye.compose(frame?, opts?)            Composition & taste metrics: saliency
 *                                             scanpath, focal dominance, Notan tonal
 *                                             massing, rest-vs-detail rhythm, Oklab
 *                                             palette tier harmony, & optional DOM
 *                                             layout grammar (rails/drifts/gaps/wraps)
 *   bro.eye.form(meshOrScene, opts?)          3D sculpture & form critic: cotangent H/K
 *                                             curvature, G1 creases, pinches, plumb-
 *                                             line stability, X-symmetry, Y-slices,
 *                                             8-angle silhouettes + kappa(s) + lighting
 *   bro.eye.compare(frame, ref, opts?)        SSIM + Oklab deltaE + exposure EMD
 *                                             + directional & perceptual deltas
 *   bro.eye.audit(opts?)                      live DOM + pixel-grounded contrast
 *                                             (WCAG/APCA), truncation, clip,
 *                                             overlap, offscreen, hit targets
 *   bro.eye.scene(scene, opts?)               3D Vulkan LDR + reversed-Z depth
 *                                             + BVH audit (coverage, silhouette
 *                                             contrast, floating, interpenetration,
 *                                             inverted normals, UV stretch, Z-fighting)
 *   bro.eye.visualize(frameOrFrames, opts?)   annotated contact sheets & heatmaps
 *                                             ('audit', 'diff', 'exposure', 'tiling',
 *                                              'depth', 'normals', 'sheet', 'saliency',
 *                                              'notan', 'rails', 'zebra', 'curvature',
 *                                              'turntable', 'raking') -> ImageData/PNG
 *   bro.eye.fit(renderFn, opts)               deterministic CMA-ES parameter search
 *                                             against a reference, target metrics,
 *                                             audit counts, or custom scorer
 *
 * SOURCES (`frame` / `reference`). Any of:
 *   - undefined / null   capture the current composited frame (main realm;
 *                        calls engine.flush() automatically)
 *   - string             image file path (png/jpg/webp/ktx2/... via broimage)
 *   - ImageData          or `{ width, height, data: Uint8ClampedArray|Uint8Array }`
 *   - ImageBitmap        decoded/transferred bitmap
 *   - SceneGraph         renders via renderForRead() and reads post-tonemap RGBA
 *
 * WORKERS. `measure`, `compare`, `visualize` (over image sources), and `fit`
 * run identically inside Worker realms. `audit` and `scene` require main-realm
 * DOM / Vulkan engine access.
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * @typedef {undefined|null|string|ImageData|ImageBitmap|Object} EyeFrameSource
 */

/**
 * @typedef {Object} EyeDominantColor
 * @property {number} r
 * @property {number} g
 * @property {number} b
 * @property {string} hex
 * @property {number} L       Oklab lightness (0..1)
 * @property {number} C       Oklab chroma
 * @property {number} h       hue angle in degrees (0..360)
 * @property {number} share   fraction of pixels (0..1)
 */

/**
 * 2D FFT periodic repetition detector.
 * @typedef {Object} EyeTiling
 * @property {number} score           0..1 (higher = stronger repeating tile artifact)
 * @property {number} peakRatio       strongest isolated off-DC harmonic peak ratio
 * @property {number} periodX         estimated repeat period X in pixels (0 if none)
 * @property {number} periodY         estimated repeat period Y in pixels (0 if none)
 * @property {number} horizontalBias  0..1
 * @property {number} verticalBias    0..1
 */

/**
 * Texture boundary wrap-around seam analysis (detects tile border discontinuities).
 * @typedef {Object} EyeSeam
 * @property {number} score           0..1 (0 = seamless wrap, >0.35 = visible border seam)
 * @property {number} hSeam           top<->bottom wrap seam score (0..1)
 * @property {number} vSeam           left<->right wrap seam score (0..1)
 * @property {number} edgeDeltaE      mean Oklab deltaE across wrapped borders
 * @property {number} interiorDeltaE  baseline adjacent-pixel Oklab deltaE inside image
 */

/**
 * Multi-scale spatial detail energy distribution across frequency bands (sums to 1).
 * @typedef {Object} EyeDetailBands
 * @property {number} low             macro / large-scale form energy share
 * @property {number} mid             medium-scale texture detail share
 * @property {number} high            fine micro-detail energy share
 */

/**
 * @typedef {Object} EyeBalance
 * @property {number} centerOfMassX   0..1 horizontal saliency center
 * @property {number} centerOfMassY   0..1 vertical saliency center
 * @property {number} leftRightDiff   -1..1 (+ = heavier right)
 * @property {number} topBottomDiff   -1..1 (+ = heavier bottom)
 * @property {number} thirdsEnergy    fraction of visual weight near rule-of-thirds points
 */

/**
 * @typedef {Object} EyeMeasurement
 * @property {number} width
 * @property {number} height
 * @property {number} pixels
 * @property {number} aspect
 * @property {number} lumaMean            Oklab L mean (0..1)
 * @property {number} lumaStd             Oklab L std dev
 * @property {number} lumaMin
 * @property {number} lumaMax
 * @property {number} lumaP05
 * @property {number} lumaP50
 * @property {number} lumaP95
 * @property {number} dynamicRange        lumaP95 - lumaP05
 * @property {number} exposureBias        lumaMean - 0.5
 * @property {number} crushedBlackRatio   fraction of nearly black pixels
 * @property {number} blownWhiteRatio     fraction of clipped white pixels
 * @property {number} bandingScore        0..1 staircase gradient banding score
 * @property {number[]} zones             10 luminance zone histogram bins summing to 1
 * @property {number} chromaMean
 * @property {number} chromaStd
 * @property {number} chromaP95
 * @property {number} colorfulRatio       fraction with Oklab C > 0.04
 * @property {number} hueEntropy          0..1 normalized 12-bin hue diversity
 * @property {number} warmCoolBalance     -1 (cool) .. +1 (warm)
 * @property {EyeDominantColor[]} dominantColors
 * @property {number} sharpness           0..1 normalized Laplacian sharpness
 * @property {number} laplacianVariance
 * @property {number} tenengrad
 * @property {number} edgeDensity         fraction of edge pixels
 * @property {number} clutterScore        0..1 visual busyness
 * @property {number} whitespaceRatio     fraction of calm low-gradient surface
 * @property {EyeTiling} tiling
 * @property {EyeSeam} seam
 * @property {EyeDetailBands} detailBands
 * @property {EyeBalance} balance
 * @property {Object[]} grid              rows x cols regional breakdown
 */

/**
 * @typedef {Object} EyeComparison
 * @property {number} score               0 for identical images; higher = farther
 * @property {boolean} sameSize
 * @property {number} ssim                1.0 = identical structure
 * @property {number} ssimDist            1 - clamp(ssim, 0, 1)
 * @property {number} deltaEMean          mean Oklab deltaE * 100
 * @property {number} deltaEP95
 * @property {number} deltaEMax
 * @property {number} changedRatio        fraction differing past threshold
 * @property {number} psnr                dB (+Infinity when equal)
 * @property {number} rms
 * @property {number} exposureDist        1D Earth Mover's Distance over luminance zones
 * @property {number} colorDist
 * @property {number} detailDist
 * @property {number} lumaDiff            frame.lumaMean - reference.lumaMean
 * @property {number} contrastRatio       frame.lumaStd / reference.lumaStd
 * @property {number} chromaRatio         frame.chromaMean / reference.chromaMean
 * @property {number} sharpnessRatio      frame.sharpness / reference.sharpness
 * @property {number} tilingDiff          frame.tiling.score - reference.tiling.score
 * @property {Object[]} cells
 */

/**
 * @typedef {Object} EyeAuditIssue
 * @property {'text-contrast'|'text-truncated'|'overflow-clip'|'overlap'|'offscreen'|'small-hit-target'|'blank-frame'|'exposure-clip'|'banding'|'tiling-artifact'|'floating'|'interpenetration'|'inverted-normals'|'uv-stretch'|'low-silhouette-contrast'|'z-fighting'|'frame-clipped'|'under-framed'|'flat-lighting'} kind
 * @property {'error'|'warning'} severity
 * @property {string} selector
 * @property {string} message
 * @property {DOMRect} rect
 * @property {number} [wcag]
 * @property {number} [apca]
 * @property {string} [fg]
 * @property {string} [bg]
 * @property {number} [overflowX]
 * @property {number} [overflowY]
 * @property {string} [otherSelector]
 * @property {number} [overlapRatio]
 * @property {string} [nodeName]
 * @property {string} [otherNodeName]
 * @property {number} [gap]
 * @property {number} [value]
 */

// ── API ──────────────────────────────────────────────────────────────────────

/**
 * Measure an image or live composited frame.
 * @param {EyeFrameSource} [frame]
 * @param {{rows?: number, cols?: number, maxColors?: number, tiling?: boolean}} [opts]
 * @returns {EyeMeasurement}
 */
bro.eye.measure = function(frame, opts) {};

/**
 * Analyze composition, saliency hierarchy, Notan tonal massing, rest-vs-detail rhythm,
 * palette tier harmony, and (on live DOM capture) layout grammar (rails, drifts, wraps, tangents).
 * @param {EyeFrameSource} [frame]
 * @param {{maxAnchors?: number, notanTones?: number, rhythmRows?: number, rhythmCols?: number, layout?: boolean, selector?: string, railTolerancePx?: number, driftMaxPx?: number}} [opts]
 * @returns {Object}
 */
bro.eye.compose = function(frame, opts) {};

/**
 * Analyze 3D sculptural form of a `Mesh` or live `SceneGraph`: intrinsic mean/Gaussian
 * curvature, G1 creases, pinches, divergence CoM & base support stability, X-symmetry,
 * Y-slices, multi-angle turntable silhouettes (negative space & contour curvature), and
 * rendered highlight/halftone/core-shadow/warm-cool turn when given a `SceneGraph`.
 * @param {Object} meshOrScene
 * @param {{focus?: string, azimuths?: number, elevation?: number, creaseAngle?: number, slices?: number, silhouetteRes?: number, render?: boolean}} [opts]
 * @returns {Object}
 */
bro.eye.form = function(meshOrScene, opts) {};

/**
 * Compare `frame` against `reference`.
 * @param {EyeFrameSource} frame
 * @param {EyeFrameSource} reference
 * @param {{rows?: number, cols?: number, threshold?: number, weights?: {ssim?: number, deltaE?: number, exposure?: number, color?: number}}} [opts]
 * @returns {EyeComparison}
 */
bro.eye.compare = function(frame, reference, opts) {};

/**
 * Audit the live document against its actually rendered pixels.
 * Checks rendered text contrast (WCAG 2.1 + APCA against underlying canvas/gradient pixels),
 * text truncation, non-scrollable overflow clipping, element overlap, offscreen content,
 * undersized interactive targets, and frame-level exposure/banding/tiling defects.
 * @param {{selector?: string, minContrast?: number, minApca?: number, minHitTarget?: number, minOverlapPx?: number, overlap?: boolean, clip?: boolean, offscreen?: boolean, hitTargets?: boolean, frame?: boolean, maxIssues?: number}} [opts]
 * @returns {{ok: boolean, score: number, errorCount: number, warningCount: number, checkedTextNodes: number, checkedElements: number, issues: EyeAuditIssue[], frame: EyeMeasurement}}
 */
bro.eye.audit = function(opts) {};

/**
 * Audit a live 3D SceneGraph using Vulkan LDR + reversed-Z depth + mesh BVH geometry.
 * Checks sky/depth range, per-mesh screen coverage, figure-ground silhouette contrast,
 * floating meshes, interpenetrating geometry, inverted normals, UV stretch, and optional
 * sub-pixel camera jitter stability (Z-fighting / shimmer).
 * @param {Object} scene
 * @param {{focus?: string, render?: boolean, checkStability?: boolean, checkFraming?: boolean, jitterPx?: number, minSilhouetteContrast?: number, floatMin?: number, floatMax?: number, penetrationEps?: number, uvStretchThreshold?: number, maxIssues?: number}} [opts]
 * @returns {{ok: boolean, score: number, errorCount: number, warningCount: number, width: number, height: number, skyRatio: number, nearDist: number, medianDist: number, farDist: number, exposureEv: number, meteredEv: number, meshDrawn: number, meshCulled: number, nodes: Object[], issues: EyeAuditIssue[], stability: Object, frame: EyeMeasurement}}
 */
bro.eye.scene = function(scene, opts) {};

/**
 * Build a labelled diagnostic contact sheet or heatmap image.
 * Modes: 'audit', 'diff', 'exposure', 'tiling', 'tile2x2', 'depth', 'normals', 'sheet'.
 * @param {EyeFrameSource|EyeFrameSource[]} frameOrFrames
 * @param {{mode?: string, layout?: 'side'|'stack'|'grid', labels?: string[], fontScale?: number, maxPanelDim?: number, path?: string}} [opts]
 * @returns {{width: number, height: number, data: Uint8ClampedArray, panels: Object[], path?: string}}
 */
bro.eye.visualize = function(frameOrFrames, opts) {};

/**
 * Deterministically optimize rendering/styling/shader parameters via CMA-ES.
 * `renderFn(paramsObj)` returns a frame source (or undefined to capture live frame).
 * @param {(params: Object) => EyeFrameSource} renderFn
 * @param {{params: {name: string, min: number, max: number, start?: number, scale?: 'linear'|'log'}[], reference?: EyeFrameSource, referenceWeight?: number, measures?: Object, auditErrorWeight?: number, auditWarningWeight?: number, scorer?: (frame: EyeFrameSource) => number, onProgress?: (evalCount: number, bestDist: number, bestParams: Object) => void, maxEvaluations?: number, targetDistance?: number, seed?: number}} opts
 * @returns {{ok: boolean, distance: number, evaluations: number, params: Object, frame: EyeMeasurement, comparison?: EyeComparison, history: number[]}}
 */
bro.eye.fit = function(renderFn, opts) {};
