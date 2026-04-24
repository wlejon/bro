// app.js — entry point
(function () {
    "use strict";

    SC.Game.init();
    window.__serpcoil = {
        fire:          function (a) { return SC.Game.hooks().fire(a); },
        chain:         function () { return SC.Game.hooks().chain(); },
        shooter:       function () { return SC.Game.hooks().shooter(); },
        path:          function () { return SC.Game.hooks().path(); },
        insertAt:      function (d, c) { return SC.Game.hooks().insertAt(d, c); },
        detectMatches: function (i) { return SC.Game.hooks().detectMatches(i); },
        seedLevel:     function (n, seed) { return SC.Game.hooks().seedLevel(n, seed); },
        score:         function () { return SC.Game.hooks().score(); },
        setScore:      function (v) { return SC.Game.hooks().setScore(v); },
        combo:         function () { return SC.Game.hooks().combo(); },
        danger:        function () { return SC.Game.hooks().danger(); },
        state:         function () { return SC.Game.state(); },
        tick:          function (dt) { return SC.Game.tick(dt); },
        currentScreen: function () { return SC.Game.Screens.currentName(); },
        forceEmpty:    function () { return SC.Game.hooks().forceEmpty(); },
        advanceChainToGoal: function () { return SC.Game.hooks().advanceChainToGoal(); },
        setChainSpeed: function (s) { return SC.Game.hooks().setChainSpeed(s); },
        switchTo:      function (n) { return SC.Game.Screens.switchTo(n); },
        awardPowerup:  function (pu) { return SC.Game.hooks().awardPowerup(pu); }
    };

    var last = performance.now();
    function frame(t) {
        requestAnimationFrame(frame);
        var dt = t - last;
        last = t;
        if (dt < 0) dt = 0;
        if (dt > 100) dt = 100;

        SC.Game.updateCanvasSize();
        SC.Game.Screens.update(dt);
        SC.Game.draw();
    }
    requestAnimationFrame(frame);

    console.log("Serpcoil loaded.");
})();
