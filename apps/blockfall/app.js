// app.js — Main entry point: canvas setup, game loop, event binding
(function() {
"use strict";

var canvas = document.getElementById("game");
var ctx = canvas.getContext("2d");
var getW = function() { return Canvas.w(ctx, 800); };
var getH = function() { return Canvas.h(ctx, 700); };

var lastFrameTime = 0;

// --- Initialize ---
T.Storage.load();
T.Audio.init();
T.Input.init();
T.Screens.init();

// --- Event binding ---
document.body.addEventListener("keydown", function(e) {
    if (e.repeat) {
        // Only allow repeat for menu screens, not gameplay
        var name = T.Screens.getName();
        if (name === "playing" || name === "countdown") return;
    }
    T.Screens.keydown(e.key);
});

document.body.addEventListener("keyup", function(e) {
    T.Screens.keyup(e.key);
});

// Action events from bro.settings
document.addEventListener("action", function(e) {
    T.Screens.onAction(e.detail.action, e.detail.phase, e.detail.key);
});

// --- Game loop ---
function gameLoop(timestamp) {
    requestAnimationFrame(gameLoop);

    var dt = timestamp - lastFrameTime;
    lastFrameTime = timestamp;
    if (dt > 100) dt = 100;
    if (dt < 0) dt = 0;

    var W = getW(), H = getH();

    T.Screens.update(dt, W, H);
    T.Audio.updateSequences();

    ctx.clearRect(0, 0, W, H);
    T.Screens.draw(ctx, W, H);
}

// --- Start ---
T.Screens.switchTo("title");
lastFrameTime = performance.now();
requestAnimationFrame(gameLoop);

console.log("Tetris loaded!");
})();
