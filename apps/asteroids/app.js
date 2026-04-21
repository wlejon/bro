// app.js — Entry point: canvas setup, event binding, game loop
(function() {
"use strict";

var canvas = document.getElementById("game");
var ctx = canvas.getContext("2d");

function getW() { return ctx.canvasWidth || canvas.width || 900; }
function getH() { return ctx.canvasHeight || canvas.height || 800; }

var lastFrameTime = 0;

// --- Init subsystems ---
A.Storage.load();
A.Audio.init();
A.Screens.init(getW(), getH());

// --- Input ---
document.body.addEventListener("keydown", function(e) {
    if (e.repeat) {
        // Allow repeat for menu navigation only
        var n = A.Screens.getName();
        if (n === "playing") return;
    }
    A.Screens.keydown(e.key, getW(), getH());
});

document.body.addEventListener("keyup", function(e) {
    A.Screens.keyup(e.key);
});

// --- Game loop ---
function gameLoop(timestamp) {
    requestAnimationFrame(gameLoop);

    var dt = timestamp - lastFrameTime;
    lastFrameTime = timestamp;
    if (dt > 100) dt = 100;
    if (dt < 0) dt = 0;

    var W = getW(), H = getH();

    A.Screens.update(dt, W, H);

    ctx.clearRect(0, 0, W, H);
    A.Screens.draw(ctx, W, H);
}

// --- Start ---
A.Screens.switchTo("title");
lastFrameTime = performance.now();
requestAnimationFrame(gameLoop);

console.log("Asteroids loaded!");
})();
