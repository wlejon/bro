/**
 * =============================================================================
 * bro.text — typography & text shaping diagnostics
 * =============================================================================
 *
 * A diagnostic window onto the render layer's text shaping and UAX #9 bidi
 * resolver. It exists so the cluster map, caret positions, and bidi runs can
 * be asserted directly from tests without a separate C++ test binary.
 *
 * This is deliberately NOT an app-facing text API:
 *   - Offsets are UTF-8 BYTES, the render layer's own domain, not UTF-16 code units.
 *     Byte offsets are what the cluster map is expressed in.
 *   - It reports what the shaper did, not what CSS says should happen.
 *
 * @example
 *   // Shape text with font options
 *   const res = bro.text.shape("Hello World", { family: "Arial", size: 16 });
 *   if (res && res.clusters) {
 *     console.log("Width: " + res.width + ", Glyph count: " + res.glyphCount);
 *     for (const c of res.clusters) {
 *       console.log("Cluster [" + c.start + ".." + c.end + "] x=" + c.x);
 *     }
 *   }
 *
 * @example
 *   // Caret cluster positioning
 *   const pos = bro.text.byteOffsetToX("Hello", { family: "Arial", size: 16 }, 2);
 *   if (pos) {
 *     console.log("Caret x: " + pos.x + ", leading: " + pos.isLeadingEdge);
 *   }
 *
 * @example
 *   // UAX #9 Bidirectional resolution
 *   const para = bro.text.bidi("Hello Arabic", "auto");
 *   if (para) {
 *     console.log("Paragraph level: " + para.paragraphLevel);
 *   }
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * Text styling options for shaping and metric queries.
 * @typedef {Object} TextOptions
 * @property {string} [family] -  Font family name (default "Arial").
 * @property {number} [size] -  Font size in pixels (default 16.0).
 * @property {number} [weight] -  Font weight (e.g. 400 for normal, 700 for bold, default 400).
 * @property {boolean} [italic] -  Italic style flag (default false).
 * @property {number} [letterSpacing] -  Letter spacing in pixels (default 0.0).
 * @property {number} [wordSpacing] -  Word spacing in pixels (default 0.0).
 */

/**
 * Single shaped glyph cluster within a shaped text run.
 * @typedef {Object} TextCluster
 * @property {number} [start] -  Starting byte offset in the source UTF-8 string.
 * @property {number} [end] -  Ending byte offset in the source UTF-8 string.
 * @property {number} [x] -  Horizontal X offset in pixels from the start of the run.
 * @property {number} [advance] -  Horizontal advance width of this cluster in pixels.
 * @property {number} [glyphs] -  Number of glyphs representing this cluster.
 * @property {boolean} [rtl] -  Right-to-left flag for this cluster.
 */

/**
 * Result of shaping a text string.
 * @typedef {Object} ShapedText
 * @property {string} [text] -  Original text string.
 * @property {number} [glyphCount] -  Total number of glyphs in the shaped run.
 * @property {number} [width] -  Total advance width in pixels.
 * @property {Array<TextCluster>} [clusters] -  Array of shaped glyph clusters.
 */

/**
 * Caret position descriptor.
 * @typedef {Object} Caret
 * @property {number} [x] -  Horizontal X position in pixels.
 * @property {boolean} [isLeadingEdge] -  Whether caret is on the leading edge of the glyph.
 */

/**
 * Primary and optional secondary caret position at direction boundaries.
 * @typedef {Object} CaretPosition
 * @property {number} [x] -  Primary caret horizontal position in pixels.
 * @property {boolean} [isLeadingEdge] -  Whether primary caret is on leading edge.
 * @property {Caret} [secondary] -  Secondary caret position at direction boundary (if present).
 */

/**
 * Byte range of a cluster.
 * @typedef {Object} ClusterRange
 * @property {number} [start] -  Starting byte offset.
 * @property {number} [end] -  Ending byte offset.
 */

/**
 * Text shaper cache hit and miss statistics.
 * @typedef {Object} TextCacheStats
 * @property {number} [hits] -  Number of cache hits.
 * @property {number} [misses] -  Number of cache misses.
 */

/**
 * Single bidirectional text run.
 * @typedef {Object} BidiRun
 * @property {number} [start] -  Starting character index.
 * @property {number} [end] -  Ending character index.
 * @property {number} [level] -  Resolved embedding level.
 */

/**
 * Paragraph-level bidirectional resolution result.
 * @typedef {Object} BidiParagraph
 * @property {number} [paragraphLevel] -  Resolved paragraph embedding level (0 = LTR, 1 = RTL).
 * @property {boolean} [uniform] -  Whether the entire paragraph has uniform directionality.
 * @property {Array<number>} [levels] -  Resolved embedding level per codepoint.
 * @property {Array<BidiRun>} [runs] -  Sequence of resolved directional runs.
 */

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * Diagnostic window onto text shaping, cluster mapping, and bidi resolution.
 */
/**
 * Whether the UAX #9 Bidirectional algorithm engine is compiled in and available.
 * @readonly
 * @type {boolean}
 */
bro.text.bidiAvailable;

/**
 * Shape a text string with font and spacing options.
 *
 * @param {string} text - UTF-8 text string to shape
 * @param {TextOptions} [options] - Font styling and spacing options
 * @returns {ShapedText|null} Shaped text result with glyph count, width, and cluster array
 */
bro.text.shape = function(text, options) {};

/**
 * Map a UTF-8 byte offset to a horizontal caret X coordinate.
 *
 * @param {string} text - UTF-8 text string
 * @param {TextOptions} [options] - Font styling options
 * @param {number} [byteOffset] - Zero-based byte offset into text
 * @returns {CaretPosition|null} Caret position object
 */
bro.text.byteOffsetToX = function(text, options, byteOffset) {};

/**
 * Map a horizontal X coordinate to the nearest UTF-8 byte offset.
 *
 * @param {string} text - UTF-8 text string
 * @param {TextOptions} [options] - Font styling options
 * @param {number} [x] - Horizontal pixel position
 * @returns {number} Nearest byte offset
 */
bro.text.xToByteOffset = function(text, options, x) {};

/**
 * Query the byte span [start, end) of the cluster covering a byte offset.
 *
 * @param {string} text - UTF-8 text string
 * @param {TextOptions} [options] - Font styling options
 * @param {number} [byteOffset] - Zero-based byte offset
 * @returns {ClusterRange|null} Cluster byte range
 */
bro.text.clusterRange = function(text, options, byteOffset) {};

/**
 * Query text shaping cache statistics.
 * @returns {TextCacheStats} Cache hit and miss counters
 */
bro.text.cacheStats = function() {};

/**
 * Resolve paragraph directionality and character embedding levels under UAX #9.
 *
 * @param {string} text - UTF-8 text string
 * @param {string} [base] - Base direction ('auto', 'ltr', or 'rtl')
 * @param {boolean} [override] - Whether directional override is enabled
 * @returns {BidiParagraph|null} Resolved paragraph structure
 */
bro.text.bidi = function(text, base, override) {};

/**
 * Reorder character levels to visual indices (UAX #9 Rule L2).
 *
 * @param {Array<number>} levels - Sequence of resolved embedding levels per character
 * @returns {Array<number>} Logical index for each visual slot
 */
bro.text.bidiReorder = function(levels) {};

