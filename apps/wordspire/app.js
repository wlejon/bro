// app.js — entry point for wordspire.
'use strict';
(function () {
    var canvas = document.getElementById('game');
    var ctx = canvas.getContext('2d');
    function Wd() { return Canvas.w(ctx, 1000); }
    function Hd() { return Canvas.h(ctx, 800);  }

    W.Audio.init();
    W.Screens.init();

    // --- Dictionary load ---
    W.Screens.switchTo('loading');
    var loadStatus = document.getElementById('loading-status');
    if (loadStatus) loadStatus.textContent = 'Reading dictionary...';
    W.Dictionary.load().then(function (n) {
        console.log('dictionary loaded:', n, 'words');
        if (loadStatus) loadStatus.textContent = n + ' words loaded.';
        W.Screens.switchTo('title');
    }).catch(function (err) {
        console.error('dictionary load failed:', err);
        if (loadStatus) loadStatus.textContent = 'ERROR: ' + (err.message || err);
        // Fall back to a tiny built-in vocabulary so the game is still playable.
        W.Dictionary._setWords([
            'cat','dog','eat','run','sun','moon','star','stone','word','play',
            'game','hello','world','tile','chain','board','spire','tower',
            'test','type','tone','crate','rate','hate','late','plate'
        ]);
        W.Screens.switchTo('title');
    });

    // Keyboard routing.
    document.body.addEventListener('keydown', function (e) {
        var name = W.Screens.manager().name();
        if (e.repeat && name === 'playing') return;
        W.Screens.manager().keydown(e.key);
    });
    document.body.addEventListener('keyup', function (e) {
        W.Screens.manager().keyup(e.key);
    });

    // Canvas mouse routing — only during gameplay.
    canvas.addEventListener('click', function (e) {
        if (W.Screens.manager().name() !== 'playing') return;
        var r = canvas.getBoundingClientRect();
        var x = e.clientX - (r ? r.left : 0);
        var y = e.clientY - (r ? r.top : 0);
        W.Board.mouseClick(x, y);
    });
    canvas.addEventListener('dblclick', function (e) {
        if (W.Screens.manager().name() !== 'playing') return;
        var r = canvas.getBoundingClientRect();
        var x = e.clientX - (r ? r.left : 0);
        var y = e.clientY - (r ? r.top : 0);
        W.Board.mouseDblClick(x, y);
    });

    var loop = GameLoop.create({
        tick: function (dt) { W.Screens.manager().update(dt, Wd(), Hd()); },
        draw: function () {
            ctx.clearRect(0, 0, Wd(), Hd());
            W.Screens.manager().draw(ctx, Wd(), Hd());
        }
    });
    loop.start();

    // --- Test hooks ---
    window.__wordspire = {
        W: W,
        board: W.Board,
        dictionary: W.Dictionary,
        scoring: W.Scoring,
        storage: W.Storage,
        screens: W.Screens,
        particles: W.Particles,
        loop: loop,
        step: function (dt) { W.Board.tick(dt || 16); },
        setGrid: function (letters) { W.Board.setGridTest(letters); },
        playPath: function (path) { return W.Board.playWordByPath(path); },
        forceBurn: function (c, r) { W.Board.forceBurnAt(c, r); },
        isValidPath: W.Board.isValidPath,
        computeWordScore: W.Scoring.computeWordScore,
        dictLookup: function (w) { return W.Dictionary.isWord(w); },
        settle: W.Board.settle,
        findMatches: function (n) { return W.Board.findMatches(n); }
    };

    console.log('Wordspire loaded.');
})();
