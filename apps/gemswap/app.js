// app.js — entry point for gemswap.
'use strict';
(function () {
    var canvas = document.getElementById('game');
    var ctx = canvas.getContext('2d');

    function W() { return Canvas.w(ctx, 900); }
    function H() { return Canvas.h(ctx, 800); }

    G.Controls.init();
    G.Screens.init();

    var loop = GameLoop.create({
        tick: function (dt) {
            var Sc = G.Screens.manager();
            Sc.update(dt, W(), H());
        },
        draw: function () {
            ctx.clearRect(0, 0, W(), H());
            var Sc = G.Screens.manager();
            Sc.draw(ctx, W(), H());
        },
    });

    G.Screens.switchTo('title');
    loop.start();

    // Expose test hooks.
    window.__gemswap = {
        G: G,
        board: G.Board,
        particles: G.Particles,
        puzzles: G.Puzzles,
        screens: G.Screens,
        loop: loop,
    };

    console.log('Gemswap loaded.');
})();
