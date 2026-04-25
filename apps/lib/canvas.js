// canvas.js — tiny canvas-size helper.
//
// The bro engine sets ctx.canvasWidth / ctx.canvasHeight on the 2D context
// (DPR-aware). Apps still want a fallback when run in a plain browser. This
// centralizes the boilerplate every app was reinventing.
//
// Usage:
//   <script src="../lib/canvas.js"></script>
//   const W = Canvas.w(ctx, 900);                    // single dimension
//   const { w, h } = Canvas.size(ctx, 900, 800);     // both at once

(function (global) {
    'use strict';

    function w(ctx, fallback) {
        return (ctx && ctx.canvasWidth)
            || (ctx && ctx.canvas && ctx.canvas.width)
            || fallback || 800;
    }

    function h(ctx, fallback) {
        return (ctx && ctx.canvasHeight)
            || (ctx && ctx.canvas && ctx.canvas.height)
            || fallback || 600;
    }

    function size(ctx, fallbackW, fallbackH) {
        return { w: w(ctx, fallbackW), h: h(ctx, fallbackH) };
    }

    global.Canvas = { w, h, size };
})(typeof globalThis !== 'undefined' ? globalThis : this);
