// app.js — Entry point: canvas setup, input wiring, game loop
(function() {
"use strict";

var canvas = document.getElementById("game");
var ctx = canvas.getContext("2d");

function getW() { return ctx.canvasWidth || canvas.width || 900; }
function getH() { return ctx.canvasHeight || canvas.height || 800; }

A.Storage.load();
A.Audio.init();

// Use the standard vocabulary so user rebindings carry across games.
// "up" doubles as thrust-in-play and menu-up on title screens.
Input.init([
    { name: "left",    label: "Rotate Left",  defaults: ["a", "ArrowLeft"] },
    { name: "right",   label: "Rotate Right", defaults: ["d", "ArrowRight"] },
    { name: "up",      label: "Thrust",       defaults: ["w", "ArrowUp"] },
    { name: "down",    label: "Menu Down",    defaults: ["s", "ArrowDown"] },
    { name: "primary", label: "Fire",         defaults: [" ", "Mouse0"] },
    { name: "confirm", label: "Confirm",      defaults: ["Enter"] },
    { name: "pause",   label: "Pause",        defaults: ["Escape", "p"] },
]);
Input.attach(window);

A.Screens.init(getW(), getH());

Input.onAction(function(action, phase) {
    if (phase !== "down" || !action) return;
    // Screen-level navigation: translate actions back to DOM key strings
    // that A.Screens.keydown already speaks (menu nav was written against them).
    var name = A.Screens.getName();
    if (name === "playing") {
        if (action === "pause") A.Screens.keydown("Escape", getW(), getH());
        return;
    }
    if (action === "up")        A.Screens.keydown("ArrowUp",   getW(), getH());
    else if (action === "down") A.Screens.keydown("ArrowDown", getW(), getH());
    else if (action === "confirm") A.Screens.keydown("Enter",  getW(), getH());
    else if (action === "pause")   A.Screens.keydown("Escape", getW(), getH());
});

A.Screens.switchTo("title");
GameLoop.create({
    tick: function(dt) { A.Screens.update(dt, getW(), getH()); },
    draw: function() {
        var W = getW(), H = getH();
        ctx.clearRect(0, 0, W, H);
        A.Screens.draw(ctx, W, H);
    },
}).start();
})();
