// text.js — thin canvas-text helpers. Uses ctx.fillText, which renders
// correctly in both windowed and bro-headless GPU screenshots.
'use strict';
var W = window.W = window.W || {};

W.Text = (function () {
    // The old API took a "scale" (bitmap-font pixel multiplier, ~7px tall per
    // unit). Map to a comparable sans-serif size so call sites stay terse.
    function sizeFor(scale) { return Math.max(12, Math.round(scale * 9)); }
    function weightFor(scale) { return scale >= 4 ? 'bold ' : ''; }
    function fontFor(scale) { return weightFor(scale) + sizeFor(scale) + 'px sans-serif'; }

    return {
        // Top-left anchor at (x, y).
        draw: function (ctx, text, x, y, scale, color) {
            ctx.save();
            ctx.font = fontFor(scale);
            ctx.fillStyle = color || '#fff';
            ctx.textAlign = 'left';
            ctx.textBaseline = 'top';
            ctx.fillText(text, x, y);
            ctx.restore();
        },
        // Centered on (x, y).
        drawCentered: function (ctx, text, x, y, scale, color) {
            ctx.save();
            ctx.font = fontFor(scale);
            ctx.fillStyle = color || '#fff';
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(text, x, y);
            ctx.restore();
        },
        // Approximate pixel width for the same font settings used by draw().
        // sans-serif avg char width ≈ 0.55 * size; bold nudges it up slightly.
        measure: function (text, scale) {
            var cw = sizeFor(scale) * (scale >= 4 ? 0.6 : 0.55);
            return text.length * cw;
        },
    };
})();
