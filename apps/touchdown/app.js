// app.js — Entry point: canvas, input wiring, main loop
(function() {
"use strict";

var canvas = document.getElementById("game");
var ctx = canvas.getContext("2d");

function getW() { return Canvas.w(ctx, 900); }
function getH() { return Canvas.h(ctx, 800); }

T.Storage.load();
T.Audio.init();

Input.init([
    { name: "left",    label: "Rotate Left",  defaults: ["a", "ArrowLeft"] },
    { name: "right",   label: "Rotate Right", defaults: ["d", "ArrowRight"] },
    { name: "thrust",  label: "Thrust",       defaults: ["w", "ArrowUp", " "] },
    { name: "up",      label: "Menu Up",      defaults: ["ArrowUp"] },
    { name: "down",    label: "Menu Down",    defaults: ["ArrowDown"] },
    { name: "confirm", label: "Confirm",      defaults: ["Enter"] },
    { name: "pause",   label: "Pause",        defaults: ["Escape", "p"] },
]);
Input.attach(window);

T.Screens.init(getW(), getH());

// Route rising-edge actions into the screen manager as DOM key strings
// — T.Screens.menuNav uses "ArrowUp"/"ArrowDown"/"Enter"/"Escape".
Input.onAction(function(action, phase) {
    if (phase !== "down" || !action) return;
    var name = T.Screens.getName();
    if (name === "playing") {
        if (action === "pause") T.Screens.keydown("Escape", getW(), getH());
        return;
    }
    if (action === "up")        T.Screens.keydown("ArrowUp",   getW(), getH());
    else if (action === "down") T.Screens.keydown("ArrowDown", getW(), getH());
    else if (action === "confirm") T.Screens.keydown("Enter",  getW(), getH());
    else if (action === "pause")   T.Screens.keydown("Escape", getW(), getH());
});

function toCanvasCoords(ev) {
    var rect = canvas.getBoundingClientRect();
    var W = getW(), H = getH();
    var sx = W / rect.width, sy = H / rect.height;
    return { x: (ev.clientX - rect.left) * sx, y: (ev.clientY - rect.top) * sy };
}
canvas.addEventListener("mousemove", function(ev) {
    var p = toCanvasCoords(ev);
    T.Game.setMouse(p.x, p.y);
});
canvas.addEventListener("mousedown", function(ev) {
    if (ev.button !== 2) return;
    var p = toCanvasCoords(ev);
    T.Game.setMouse(p.x, p.y, true);
});
window.addEventListener("mouseup", function(ev) {
    if (ev.button !== 2) return;
    T.Game.setMouse(undefined, undefined, false);
});
canvas.addEventListener("contextmenu", function(ev) { ev.preventDefault(); });

T.Screens.switchTo("title");
GameLoop.create({
    tick: function(dt) { T.Screens.update(dt, getW(), getH()); },
    draw: function() {
        var W = getW(), H = getH();
        ctx.clearRect(0, 0, W, H);
        T.Screens.draw(ctx, W, H);
    },
}).start();
})();
