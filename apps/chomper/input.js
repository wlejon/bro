// input.js — direction queue backed by lib/input (bro.settings rebindable).
var P = P || {};

P.Input = (function() {
    var pendingDir = -1;

    Input.init([
        { name: "up",      label: "Up",      defaults: ["w", "ArrowUp"] },
        { name: "down",    label: "Down",    defaults: ["s", "ArrowDown"] },
        { name: "left",    label: "Left",    defaults: ["a", "ArrowLeft"] },
        { name: "right",   label: "Right",   defaults: ["d", "ArrowRight"] },
        { name: "confirm", label: "Confirm", defaults: ["Enter", " "] },
        { name: "pause",   label: "Pause",   defaults: ["Escape", "p"] },
    ]);
    Input.attach(window);

    Input.onAction(function(action, phase) {
        if (phase !== "down") return;
        if (action === "right") pendingDir = 0;
        else if (action === "left")  pendingDir = 1;
        else if (action === "up")    pendingDir = 2;
        else if (action === "down")  pendingDir = 3;
    });

    return {
        consume: function() { var d = pendingDir; pendingDir = -1; return d; },
        reset:   function() { pendingDir = -1; },
    };
})();
