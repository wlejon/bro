// text.js — thin canvas-text helpers. Uses ctx.fillText, which renders
// correctly in both windowed and bro-headless GPU screenshots.
'use strict';
var W = window.W = window.W || {};

W.Text = (function () {
    function sizeFor(scale) { return Math.max(12, Math.round(scale * 9)); }
    function weightFor(scale) { return scale >= 4 ? 'bold ' : ''; }
    function fontFor(scale) { return weightFor(scale) + sizeFor(scale) + 'px sans-serif'; }

    return {
        draw: function (ctx, text, x, y, scale, color) {
            ctx.save();
            ctx.font = fontFor(scale);
            ctx.fillStyle = color || '#fff';
            ctx.textAlign = 'left';
            ctx.textBaseline = 'top';
            ctx.fillText(text, x, y);
            ctx.restore();
        },
        drawCentered: function (ctx, text, x, y, scale, color) {
            ctx.save();
            ctx.font = fontFor(scale);
            ctx.fillStyle = color || '#fff';
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(text, x, y);
            ctx.restore();
        },
        measure: function (text, scale) {
            var cw = sizeFor(scale) * (scale >= 4 ? 0.6 : 0.55);
            return text.length * cw;
        },
    };
})();
