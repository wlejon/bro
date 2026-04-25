// app.js — entry point for blockpop.
'use strict';
(function () {
    var canvas = document.getElementById('game');
    var ctx = canvas.getContext('2d');
    function W() { return Canvas.w(ctx, 900); }
    function H() { return Canvas.h(ctx, 800); }

    // Audio (may fail silently in headless/no-audio environments).
    G.Audio.init();

    G.Screens.init();

    // Keyboard routing.
    document.body.addEventListener('keydown', function (e) {
        var name = G.Screens.manager().name();
        if (e.repeat && name === 'playing') return;
        G.Screens.manager().keydown(e.key);
    });
    document.body.addEventListener('keyup', function (e) {
        G.Screens.manager().keyup(e.key);
    });

    // Canvas mouse routing — only when playing.
    canvas.addEventListener('click', function (e) {
        if (G.Screens.manager().name() !== 'playing') return;
        var r = canvas.getBoundingClientRect();
        var x = e.clientX - (r ? r.left : 0);
        var y = e.clientY - (r ? r.top : 0);
        G.Board.mouseClick(x, y);
    });
    canvas.addEventListener('wheel', function (e) {
        if (G.Screens.manager().name() !== 'playing') return;
        G.Board.mouseWheel(e.deltaY || 0);
    }, { passive: true });

    var loop = GameLoop.create({
        tick: function (dt) { G.Screens.manager().update(dt, W(), H()); },
        draw: function () {
            ctx.clearRect(0, 0, W(), H());
            G.Screens.manager().draw(ctx, W(), H());
        }
    });

    G.Screens.switchTo('title');
    loop.start();

    // Test hooks — mirror the spec.
    window.__blockpop = {
        G: G,
        board: G.Board,
        particles: G.Particles,
        screens: G.Screens,
        loop: loop,
        // High-level test helpers exactly as spec'd
        step: function (dt) { G.Board.tick(dt || 16); G.Particles.update(dt || 16); },
        pick: function () { return G.Board.pick(); },
        place: function () { return G.Board.place(); },
        moveTo: function (col) { return G.Board.moveTo(col); },
        spawnRow: function () { return G.Board.spawnRow(G.Board.getBoard(), Math.random); },
        findChains: function () { return G.Board.findChains(G.Board.getBoard()); }
    };

    console.log('Blockpop loaded.');
})();
