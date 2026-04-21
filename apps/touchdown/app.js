// app.js — Entry point: canvas, event binding, main loop
(function() {
"use strict";

var canvas = document.getElementById("game");
var ctx = canvas.getContext("2d");

function getW() { return ctx.canvasWidth || canvas.width || 900; }
function getH() { return ctx.canvasHeight || canvas.height || 800; }

var lastFrameTime = 0;

T.Storage.load();
T.Audio.init();
T.Screens.init(getW(), getH());

document.body.addEventListener("keydown", function(e) {
    if (e.repeat) {
        var n = T.Screens.getName();
        if (n === "playing") return;
    }
    T.Screens.keydown(e.key, getW(), getH());
});

document.body.addEventListener("keyup", function(e) {
    T.Screens.keyup(e.key);
});

function gameLoop(timestamp) {
    requestAnimationFrame(gameLoop);

    var dt = timestamp - lastFrameTime;
    lastFrameTime = timestamp;
    if (dt > 100) dt = 100;
    if (dt < 0) dt = 0;

    var W = getW(), H = getH();

    T.Screens.update(dt, W, H);

    ctx.clearRect(0, 0, W, H);
    T.Screens.draw(ctx, W, H);
}

T.Screens.switchTo("title");
lastFrameTime = performance.now();
requestAnimationFrame(gameLoop);

console.log("Touchdown loaded!");
})();
