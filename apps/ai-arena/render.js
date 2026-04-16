// render.js — transient FX state + advance (damage numbers, explosion rings).
//
// Phase 1 shrinks this to the lifecycle-only pieces loop.js still drives from
// world.events. The actual visuals for projectiles, units, gizmos, and FX are
// moving to scene_setup.js in Phases 5 and 6 — until then, FX are recorded
// here and not yet visible in the 3D scene.
var Render = {};
(function () {
    "use strict";

    Render.fx = {
        rings: [],   // { x, z, t, maxT, r }
        floats: [],  // { x, z, text, color, t }
    };

    Render.addExplosion = function (x, z, radius) {
        Render.fx.rings.push({ x: x, z: z, t: 0, maxT: 0.5, r: radius });
    };
    Render.addDamageNumber = function (x, z, amount, color) {
        Render.fx.floats.push({
            x: x, z: z, text: (amount | 0).toString(),
            color: color || "#ffd24a", t: 0,
        });
    };
    Render.clearFx = function () {
        Render.fx.rings.length = 0;
        Render.fx.floats.length = 0;
    };

    // Advance FX timers and reap expired entries. Phase 5 will add a second
    // pass that writes these as scene nodes / DOM overlays.
    Render.tickFx = function (dt) {
        for (var i = Render.fx.rings.length - 1; i >= 0; i--) {
            var r = Render.fx.rings[i];
            r.t += dt;
            if (r.t >= r.maxT) Render.fx.rings.splice(i, 1);
        }
        for (var j = Render.fx.floats.length - 1; j >= 0; j--) {
            var f = Render.fx.floats[j];
            f.t += dt;
            if (f.t >= 1.0) Render.fx.floats.splice(j, 1);
        }
    };
})();
