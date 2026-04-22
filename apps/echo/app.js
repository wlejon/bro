// app.js - Echo (Simon-style memory game)

var E = {
    // screen states
    STATE_TITLE: "title",
    STATE_HOWTO: "howto",
    STATE_WATCH: "watch",   // CPU playing sequence
    STATE_INPUT: "input",   // player's turn
    STATE_GAMEOVER: "gameover",

    // 4 pads: 0=red(TL), 1=green(TR), 2=yellow(BL), 3=blue(BR)
    PAD_COLORS: [
        { dim: "#7a1f22", lit: "#ff5a5f", glow: "#ff8a8f" }, // red
        { dim: "#1f6f2a", lit: "#4ade80", glow: "#86f0b0" }, // green
        { dim: "#7a6520", lit: "#f0c674", glow: "#ffe0a0" }, // yellow
        { dim: "#1f3a7a", lit: "#5b8def", glow: "#9abbff" }  // blue
    ],
    PAD_FREQS: [329.63, 415.30, 277.18, 220.00], // E4, G#4, C#4, A3

    canvas: null,
    ctx2d: null,
    w: 700,
    h: 800,

    state: "title",
    sequence: [],        // array of pad indices
    playerStep: 0,
    round: 0,
    best: 0,

    // visual glow 0..1 per pad
    padGlow: [0, 0, 0, 0],

    // watch timing
    watchTimer: null,
    watchIndex: 0,

    audio: null,
    sfxBus: -1,

    lastTime: 0,
    lastPadIndex: -1, // track which pad mouse is held on
};

// ---------------- Audio ----------------

E.initAudio = function() {
    try { this.audio = new AudioContext(); } catch(e) { this.audio = null; return; }
    try {
        this.sfxBus = this.audio.createBus();
        this.audio.setBusGain(this.sfxBus, 0.8);
        this.audio.setBusReverbEnabled(this.sfxBus, true);
        this.audio.setBusReverbRoomSize(this.sfxBus, 0.4);
        this.audio.setBusReverbMix(this.sfxBus, 0.2);
    } catch(e) { this.sfxBus = -1; }
};

E.playPadTone = function(padIdx, duration) {
    if (!this.audio) return;
    var freq = this.PAD_FREQS[padIdx];
    var dur = duration || 0.35;
    try {
        var id = this.audio.createVoice();
        this.audio.setVoiceWaveform(id, "triangle");
        this.audio.setVoiceFrequency(id, freq);
        this.audio.setVoiceGain(id, 12.0);
        this.audio.setVoiceAttack(id, 0.005);
        this.audio.setVoiceDecay(id, dur * 0.4);
        this.audio.setVoiceSustain(id, 0.6);
        this.audio.setVoiceRelease(id, 0.1);
        if (this.sfxBus !== -1) this.audio.setVoiceBus(id, this.sfxBus);
        var t = this.audio.currentTime;
        this.audio.startVoice(id, t);
        this.audio.stopVoice(id, t + dur);
    } catch(e) {}
};

E.playWrongTone = function() {
    if (!this.audio) return;
    try {
        var id = this.audio.createVoice();
        this.audio.setVoiceWaveform(id, "sawtooth");
        this.audio.setVoiceFrequency(id, 110);
        this.audio.setVoiceGain(id, 10.0);
        this.audio.setVoiceAttack(id, 0.005);
        this.audio.setVoiceDecay(id, 0.3);
        this.audio.setVoiceSustain(id, 0.2);
        this.audio.setVoiceRelease(id, 0.4);
        if (this.sfxBus !== -1) this.audio.setVoiceBus(id, this.sfxBus);
        var t = this.audio.currentTime;
        this.audio.startVoice(id, t);
        this.audio.stopVoice(id, t + 0.7);
    } catch(e) {}
};

// ---------------- Storage ----------------

E._store = Storage.create("echo");
E.loadBest = function() { this._store.load({ best: 0 }); this.best = this._store.get("best") || 0; };
E.saveBest = function() { this._store.set("best", this.best); this._store.save(); };

// ---------------- Geometry ----------------

E.getPadRect = function(padIdx) {
    // 2x2 grid, centered, big
    var margin = 60;
    var gap = 16;
    var topOffset = 120; // leave room for HUD
    var bottomOffset = 40;
    var boardW = this.w - margin * 2;
    var boardH = this.h - topOffset - bottomOffset;
    var padW = (boardW - gap) / 2;
    var padH = (boardH - gap) / 2;
    var col = padIdx % 2;
    var row = Math.floor(padIdx / 2);
    return {
        x: margin + col * (padW + gap),
        y: topOffset + row * (padH + gap),
        w: padW,
        h: padH
    };
};

E.padAt = function(mx, my) {
    for (var i = 0; i < 4; i++) {
        var r = this.getPadRect(i);
        if (mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h) return i;
    }
    return -1;
};

// ---------------- Rendering ----------------

E.resize = function() {
    // bro canvas doesn't reliably report clientWidth/Height at init; fall back
    // to the context's canvasWidth/Height (engine-provided) or window dims.
    var w = window.innerWidth || this.ctx2d.canvasWidth || 700;
    var h = window.innerHeight || this.ctx2d.canvasHeight || 800;
    this.w = w;
    this.h = h;
    this.canvas.width = w;
    this.canvas.height = h;
};

E.render = function() {
    var ctx = this.ctx2d;
    ctx.fillStyle = "#0a0e14";
    ctx.fillRect(0, 0, this.w, this.h);

    for (var i = 0; i < 4; i++) {
        var r = this.getPadRect(i);
        var g = this.padGlow[i]; // 0..1
        var col = this.PAD_COLORS[i];
        var base = col.dim;
        // interpolate toward glow color
        var fill = this.lerpColor(base, col.glow, g);
        ctx.fillStyle = fill;
        this.roundRect(ctx, r.x, r.y, r.w, r.h, 24);
        ctx.fill();

        // outer soft glow when lit
        if (g > 0.02) {
            ctx.save();
            ctx.globalAlpha = g * 0.5;
            ctx.strokeStyle = col.glow;
            ctx.lineWidth = 6;
            this.roundRect(ctx, r.x - 2, r.y - 2, r.w + 4, r.h + 4, 26);
            ctx.stroke();
            ctx.restore();
        }

        // border
        ctx.strokeStyle = "#0a0e14";
        ctx.lineWidth = 4;
        this.roundRect(ctx, r.x, r.y, r.w, r.h, 24);
        ctx.stroke();
    }
};

E.roundRect = function(ctx, x, y, w, h, r) {
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + w, y, x + w, y + h, r);
    ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r);
    ctx.arcTo(x, y, x + w, y, r);
    ctx.closePath();
};

E.lerpColor = function(a, b, t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    var ar = parseInt(a.substr(1, 2), 16), ag = parseInt(a.substr(3, 2), 16), ab = parseInt(a.substr(5, 2), 16);
    var br = parseInt(b.substr(1, 2), 16), bg = parseInt(b.substr(3, 2), 16), bb = parseInt(b.substr(5, 2), 16);
    var r = Math.round(ar + (br - ar) * t);
    var g = Math.round(ag + (bg - ag) * t);
    var bl = Math.round(ab + (bb - ab) * t);
    return "rgb(" + r + "," + g + "," + bl + ")";
};

// ---------------- Game flow ----------------

E.showScreen = function(name) {
    var screens = document.querySelectorAll(".screen");
    for (var i = 0; i < screens.length; i++) {
        screens[i].style.display = "none";
    }
    var el = document.getElementById("screen-" + name);
    if (el) el.style.display = "flex";
};

E.hideAllScreens = function() {
    var screens = document.querySelectorAll(".screen");
    for (var i = 0; i < screens.length; i++) {
        screens[i].style.display = "none";
    }
};

E.setStatus = function(text) {
    var el = document.getElementById("hud-status");
    if (el) el.textContent = text;
};

E.updateHud = function() {
    document.getElementById("hud-round").textContent = String(this.round);
    document.getElementById("hud-best").textContent = String(this.best);
};

E.startGame = function() {
    this.sequence = [];
    this.round = 0;
    this.playerStep = 0;
    this.padGlow = [0, 0, 0, 0];
    this.hideAllScreens();
    document.getElementById("hud").style.display = "block";
    this.updateHud();
    this.nextRound();
};

E.nextRound = function() {
    this.round++;
    this.sequence.push(Math.floor(Math.random() * 4));
    this.playerStep = 0;
    this.updateHud();
    this.state = E.STATE_WATCH;
    this.setStatus("WATCH");
    this.watchIndex = 0;
    // brief pause before CPU plays
    var self = this;
    setTimeout(function() { self.playWatchStep(); }, 600);
};

E.getFlashDuration = function() {
    // Speeds up as sequence grows: 600ms at round 1 -> 220ms at round 20+
    var base = 600 - (this.sequence.length - 1) * 25;
    if (base < 220) base = 220;
    return base;
};

E.playWatchStep = function() {
    if (this.state !== E.STATE_WATCH) return;
    if (this.watchIndex >= this.sequence.length) {
        // done watching - player's turn
        this.state = E.STATE_INPUT;
        this.setStatus("YOUR TURN");
        return;
    }
    var padIdx = this.sequence[this.watchIndex];
    var dur = this.getFlashDuration();
    this.flashPad(padIdx, dur / 1000);
    this.watchIndex++;
    var self = this;
    var gap = Math.max(80, Math.floor(dur * 0.35));
    setTimeout(function() { self.playWatchStep(); }, dur + gap);
};

E.flashPad = function(padIdx, durSec) {
    // visual + audio pulse
    this.padGlow[padIdx] = 1.0;
    this.playPadTone(padIdx, durSec);
};

E.handlePlayerPress = function(padIdx) {
    if (this.state !== E.STATE_INPUT) return;
    // flash + tone
    this.flashPad(padIdx, 0.3);

    if (this.sequence[this.playerStep] === padIdx) {
        this.playerStep++;
        if (this.playerStep >= this.sequence.length) {
            // completed round
            this.state = E.STATE_WATCH;
            this.setStatus("NICE!");
            var self = this;
            setTimeout(function() { self.nextRound(); }, 700);
        }
    } else {
        // wrong!
        this.gameOver();
    }
};

E.gameOver = function() {
    this.state = E.STATE_GAMEOVER;
    this.playWrongTone();
    var reached = this.round - 1; // they failed on the current round, longest completed was round-1
    if (reached < 0) reached = 0;
    if (reached > this.best) {
        this.best = reached;
        this.saveBest();
    }
    this.updateHud();

    var isNew = (reached > 0 && reached === this.best);
    var msg = "<div>Longest sequence</div>";
    msg += "<span class=\"hs-big\">" + reached + "</span>";
    if (isNew) msg += "<div style=\"color:#f0c674\">NEW BEST!</div>";
    else msg += "<div>Best: " + this.best + "</div>";
    document.getElementById("gameover-stats").innerHTML = msg;

    document.getElementById("hud").style.display = "none";
    var self = this;
    setTimeout(function() { self.showScreen("gameover"); }, 900);
};

E.goToTitle = function() {
    this.state = E.STATE_TITLE;
    this.padGlow = [0, 0, 0, 0];
    document.getElementById("hud").style.display = "none";
    this.showScreen("title");
};

// ---------------- Input ----------------

E.setupInput = function() {
    var self = this;

    Input.init([
        { name: "pad0",  label: "Pad 1 (Red)",    defaults: ["1", "q"] },
        { name: "pad1",  label: "Pad 2 (Green)",  defaults: ["2", "w"] },
        { name: "pad2",  label: "Pad 3 (Yellow)", defaults: ["3", "a"] },
        { name: "pad3",  label: "Pad 4 (Blue)",   defaults: ["4", "s"] },
        { name: "pause", label: "Quit to Title",  defaults: ["Escape"] },
    ]);
    Input.attach(window);

    this.canvas.addEventListener("mousedown", function(ev) {
        var rect = self.canvas.getBoundingClientRect();
        var mx = ev.clientX - rect.left;
        var my = ev.clientY - rect.top;
        var pad = self.padAt(mx, my);
        if (pad >= 0) {
            self.lastPadIndex = pad;
            self.handlePlayerPress(pad);
        }
    });

    Input.onAction(function(action, phase) {
        if (phase !== "down" || !action) return;
        if (self.state === E.STATE_INPUT) {
            if (action === "pad0") { self.handlePlayerPress(0); return; }
            if (action === "pad1") { self.handlePlayerPress(1); return; }
            if (action === "pad2") { self.handlePlayerPress(2); return; }
            if (action === "pad3") { self.handlePlayerPress(3); return; }
        }
        if (action === "pause" &&
            (self.state === E.STATE_INPUT || self.state === E.STATE_WATCH)) {
            self.goToTitle();
        }
    });

    var items = document.querySelectorAll(".menu-item");
    for (var i = 0; i < items.length; i++) {
        items[i].addEventListener("click", function(ev) {
            var action = this.getAttribute("data-action");
            self.handleMenuAction(action);
        });
    }
};

E.handleMenuAction = function(action) {
    if (action === "play") {
        this.startGame();
    } else if (action === "howto") {
        this.showScreen("howto");
    } else if (action === "back") {
        this.showScreen("title");
    } else if (action === "title") {
        this.goToTitle();
    }
};

// ---------------- Main loop ----------------

E.tick = function(now) {
    var dt = (now - this.lastTime) / 1000;
    if (dt > 0.1) dt = 0.1;
    this.lastTime = now;

    this.resize();

    // decay pad glows
    var decay = dt * 3.0; // fade rate
    for (var i = 0; i < 4; i++) {
        this.padGlow[i] -= decay;
        if (this.padGlow[i] < 0) this.padGlow[i] = 0;
    }

    this.render();
    var self = this;
    requestAnimationFrame(function(t) { self.tick(t); });
};

E.init = function() {
    this.canvas = document.getElementById("game");
    this.ctx2d = this.canvas.getContext("2d");
    this.resize();
    window.addEventListener("resize", function() { E.resize(); });

    this.loadBest();
    this.initAudio();
    this.setupInput();

    this.updateHud();
    this.goToTitle();

    this.lastTime = performance.now();
    var self = this;
    requestAnimationFrame(function(t) { self.tick(t); });
};

E.init();
