// ============================================================
// Tetris — Full-featured implementation
// Canvas 2D rendering, Web Audio API sounds, localStorage settings
// Browser-compatible (runs in bro engine or any modern browser)
// ============================================================

(function() {
"use strict";

// --- Canvas setup ---
var canvas = document.getElementById("game");
var ctx = canvas.getContext("2d");
// bro engine uses canvasWidth/canvasHeight; browsers use canvas.width/height
var getW = function() { return ctx.canvasWidth || canvas.width || 800; };
var getH = function() { return ctx.canvasHeight || canvas.height || 600; };

// --- Constants ---
var COLS = 10, ROWS = 20;
var STATE_MENU = 0, STATE_PLAYING = 1, STATE_PAUSED = 2, STATE_GAMEOVER = 3;

// Piece shapes: [type 1-7][4 rotations][cells as [row,col]]
var PIECES = [
    null,
    // I
    [[[1,0],[1,1],[1,2],[1,3]], [[0,2],[1,2],[2,2],[3,2]],
     [[2,0],[2,1],[2,2],[2,3]], [[0,1],[1,1],[2,1],[3,1]]],
    // O
    [[[0,1],[0,2],[1,1],[1,2]], [[0,1],[0,2],[1,1],[1,2]],
     [[0,1],[0,2],[1,1],[1,2]], [[0,1],[0,2],[1,1],[1,2]]],
    // T
    [[[0,1],[1,0],[1,1],[1,2]], [[0,1],[1,1],[1,2],[2,1]],
     [[1,0],[1,1],[1,2],[2,1]], [[0,1],[1,0],[1,1],[2,1]]],
    // S
    [[[0,1],[0,2],[1,0],[1,1]], [[0,1],[1,1],[1,2],[2,2]],
     [[1,1],[1,2],[2,0],[2,1]], [[0,0],[1,0],[1,1],[2,1]]],
    // Z
    [[[0,0],[0,1],[1,1],[1,2]], [[0,2],[1,1],[1,2],[2,1]],
     [[1,0],[1,1],[2,1],[2,2]], [[0,1],[1,0],[1,1],[2,0]]],
    // J
    [[[0,0],[1,0],[1,1],[1,2]], [[0,1],[0,2],[1,1],[2,1]],
     [[1,0],[1,1],[1,2],[2,2]], [[0,1],[1,1],[2,0],[2,1]]],
    // L
    [[[0,2],[1,0],[1,1],[1,2]], [[0,1],[1,1],[2,1],[2,2]],
     [[1,0],[1,1],[1,2],[2,0]], [[0,0],[0,1],[1,1],[2,1]]]
];

// SRS wall kick data
var KICKS = {
    normal: [
        // 0→1, 1→2, 2→3, 3→0
        [[0,0],[-1,0],[-1,1],[0,-2],[-1,-2]],
        [[0,0],[1,0],[1,-1],[0,2],[1,2]],
        [[0,0],[1,0],[1,1],[0,-2],[1,-2]],
        [[0,0],[-1,0],[-1,-1],[0,2],[-1,2]]
    ],
    I: [
        [[0,0],[-2,0],[1,0],[-2,-1],[1,2]],
        [[0,0],[-1,0],[2,0],[-1,2],[2,-1]],
        [[0,0],[2,0],[-1,0],[2,1],[-1,-2]],
        [[0,0],[1,0],[-2,0],[1,-2],[-2,1]]
    ]
};

// Colors
var COLORS = [
    null,
    "#00e5ff", // I - cyan
    "#ffd600", // O - yellow
    "#aa00ff", // T - purple
    "#00e676", // S - green
    "#ff1744", // Z - red
    "#2979ff", // J - blue
    "#ff9100"  // L - orange
];
var COLORS_LIGHT = [
    null,
    "#4df0ff", "#ffeb3b", "#d050ff", "#69f0ae", "#ff5252", "#448aff", "#ffab40"
];
var COLORS_DARK = [
    null,
    "#006978", "#7f6b00", "#55007f", "#007a3b", "#7f0b22", "#143f7f", "#7f4800"
];

// --- Settings (persisted) ---
var settings = {
    startLevel: 1,
    sfxVol: 80,
    musicVol: 70,
    ghostPiece: true,
    gridLines: true
};

// --- Controls (persisted, remappable) ---
var CONTROL_NAMES = [
    "moveLeft", "moveRight", "softDrop", "hardDrop",
    "rotCW", "rotCCW", "hold", "pause"
];
var CONTROL_LABELS = {
    moveLeft: "Move Left",
    moveRight: "Move Right",
    softDrop: "Soft Drop",
    hardDrop: "Hard Drop",
    rotCW: "Rotate CW",
    rotCCW: "Rotate CCW",
    hold: "Hold",
    pause: "Pause"
};
var DEFAULT_CONTROLS = {
    moveLeft: "ArrowLeft",
    moveRight: "ArrowRight",
    softDrop: "ArrowDown",
    hardDrop: " ",
    rotCW: "ArrowUp",
    rotCCW: "z",
    hold: "c",
    pause: "Escape"
};
var controls = {};

function loadSettings() {
    try {
        var s = localStorage.getItem("tetris_settings");
        if (s) {
            var parsed = JSON.parse(s);
            for (var k in parsed) {
                if (settings.hasOwnProperty(k)) settings[k] = parsed[k];
            }
        }
        var c = localStorage.getItem("tetris_controls");
        if (c) {
            controls = JSON.parse(c);
        } else {
            controls = JSON.parse(JSON.stringify(DEFAULT_CONTROLS));
        }
    } catch(e) {
        controls = JSON.parse(JSON.stringify(DEFAULT_CONTROLS));
    }
}

function saveSettings() {
    try {
        localStorage.setItem("tetris_settings", JSON.stringify(settings));
        localStorage.setItem("tetris_controls", JSON.stringify(controls));
    } catch(e) {}
}

// --- JSON polyfill for bro engine (QuickJS has JSON built-in) ---
// (No polyfill needed — QuickJS supports JSON natively)

// --- Audio (broaudio engine) ---
var audioCtx = null;
var musicBus = -1;
var sfxBus = -1;
var melodyAlloc = null;
var bassAlloc = null;
var percAlloc = null;
var melodySeq = null;
var bassSeq = null;
var percSeq = null;
var currentSongIndex = -1;
var musicPlaying = false;

// --- Song data: 3 original chiptune songs ---
// Each note: [beat, midiNote, velocity, duration]

var SONGS = [
    // Song A: "Block March" - E minor, driving 4/4 march feel
    // A section (bars 1-4): main theme
    // B section (bars 5-8): contrasting descent, builds back to resolve on E
    {
        name: "Block March", baseBPM: 140, loopBeats: 32,
        melodyWave: "square",
        bassWave: "triangle",
        melody: [
            // -- A section (beats 0-15) --
            [0, 76, 0.9, 1],      // E5
            [1, 71, 0.8, 0.5],    // B4
            [1.5, 72, 0.8, 0.5],  // C5
            [2, 74, 0.9, 1],      // D5
            [3, 72, 0.8, 0.5],    // C5
            [3.5, 71, 0.8, 0.5],  // B4
            [4, 69, 0.9, 1],      // A4
            [5, 69, 0.7, 0.5],    // A4
            [5.5, 72, 0.8, 0.5],  // C5
            [6, 76, 0.9, 1],      // E5
            [7, 74, 0.8, 0.5],    // D5
            [7.5, 72, 0.8, 0.5],  // C5
            [8, 71, 0.9, 1.5],    // B4
            [9.5, 72, 0.7, 0.5],  // C5
            [10, 74, 0.9, 1],     // D5
            [11, 76, 0.9, 1],     // E5
            [12, 72, 0.9, 1],     // C5
            [13, 69, 0.9, 1],     // A4
            [14, 69, 0.9, 1.75],  // A4
            // -- B section (beats 16-31) --
            [16, 74, 0.9, 1],     // D5
            [17, 76, 0.9, 0.5],   // E5
            [17.5, 74, 0.8, 0.5], // D5
            [18, 72, 0.9, 1],     // C5
            [19, 71, 0.7, 0.5],   // B4
            [19.5, 69, 0.8, 0.5], // A4
            [20, 71, 0.9, 1],     // B4
            [21, 72, 0.8, 0.5],   // C5
            [21.5, 74, 0.8, 0.5], // D5
            [22, 76, 0.9, 1.5],   // E5
            [23.5, 74, 0.7, 0.5], // D5
            [24, 72, 0.9, 1],     // C5
            [25, 69, 0.8, 0.5],   // A4
            [25.5, 69, 0.7, 0.5], // A4
            [26, 71, 0.9, 1],     // B4
            [27, 67, 0.8, 0.5],   // G4
            [27.5, 69, 0.8, 0.5], // A4
            [28, 71, 0.9, 1],     // B4
            [29, 72, 0.8, 0.5],   // C5
            [29.5, 71, 0.7, 0.5], // B4
            [30, 76, 0.9, 1.75]   // E5 (resolve)
        ],
        bass: [
            // -- A section --
            [0, 40, 0.7, 1],     // E2
            [1, 40, 0.5, 1],
            [2, 38, 0.7, 1],     // D2
            [3, 38, 0.5, 1],
            [4, 45, 0.7, 1],     // A2
            [5, 45, 0.5, 1],
            [6, 40, 0.7, 1],     // E2
            [7, 40, 0.5, 1],
            [8, 47, 0.7, 1],     // B2
            [9, 47, 0.5, 1],
            [10, 38, 0.7, 1],    // D2
            [11, 38, 0.5, 1],
            [12, 45, 0.7, 1],    // A2
            [13, 45, 0.5, 1],
            [14, 40, 0.7, 1.75], // E2
            // -- B section --
            [16, 38, 0.7, 1],    // D2
            [17, 38, 0.5, 1],
            [18, 36, 0.7, 1],    // C2
            [19, 36, 0.5, 1],
            [20, 47, 0.7, 1],    // B2
            [21, 47, 0.5, 1],
            [22, 40, 0.7, 1],    // E2
            [23, 40, 0.5, 1],
            [24, 36, 0.7, 1],    // C2
            [25, 36, 0.5, 1],
            [26, 47, 0.7, 1],    // B2
            [27, 47, 0.5, 1],
            [28, 47, 0.7, 1],    // B2
            [29, 47, 0.5, 1],
            [30, 40, 0.7, 1.75]  // E2 (resolve)
        ],
        perc: [
            // -- A section --
            [0, 36, 0.9, 0.25],   // kick
            [2, 36, 0.9, 0.25],
            [4, 36, 0.9, 0.25],
            [6, 36, 0.9, 0.25],
            [8, 36, 0.9, 0.25],
            [10, 36, 0.9, 0.25],
            [12, 36, 0.9, 0.25],
            [14, 36, 0.9, 0.25],
            [1, 42, 0.6, 0.15],   // hi-hat
            [3, 42, 0.6, 0.15],
            [5, 42, 0.6, 0.15],
            [7, 42, 0.6, 0.15],
            [9, 42, 0.6, 0.15],
            [11, 42, 0.6, 0.15],
            [13, 42, 0.6, 0.15],
            [15, 42, 0.6, 0.15],
            // -- B section --
            [16, 36, 0.9, 0.25],
            [18, 36, 0.9, 0.25],
            [20, 36, 0.9, 0.25],
            [22, 36, 0.9, 0.25],
            [24, 36, 0.9, 0.25],
            [26, 36, 0.9, 0.25],
            [28, 36, 0.9, 0.25],
            [30, 36, 0.9, 0.25],
            [17, 42, 0.6, 0.15],
            [19, 42, 0.6, 0.15],
            [21, 42, 0.6, 0.15],
            [23, 42, 0.6, 0.15],
            [25, 42, 0.6, 0.15],
            [27, 42, 0.6, 0.15],
            [29, 42, 0.6, 0.15],
            [31, 42, 0.6, 0.15]
        ]
    },
    // Song B: "Crystal Stack" - C major, bouncy and playful
    {
        name: "Crystal Stack", baseBPM: 128, loopBeats: 16,
        melodyWave: "square",
        bassWave: "triangle",
        melody: [
            [0, 72, 0.8, 0.75],   // C5
            [0.75, 74, 0.7, 0.25],// D5
            [1, 76, 0.9, 1],      // E5
            [2, 72, 0.8, 0.5],    // C5
            [2.5, 74, 0.7, 0.5],  // D5
            [3, 76, 0.8, 0.5],    // E5
            [3.5, 79, 0.9, 0.5],  // G5
            [4, 81, 0.9, 1.5],    // A5
            [5.5, 79, 0.7, 0.5],  // G5
            [6, 76, 0.8, 1],      // E5
            [7, 74, 0.7, 0.5],    // D5
            [7.5, 72, 0.7, 0.5],  // C5
            [8, 74, 0.9, 1],      // D5
            [9, 72, 0.7, 0.5],    // C5
            [9.5, 69, 0.8, 0.5],  // A4
            [10, 72, 0.9, 1],     // C5
            [11, 74, 0.8, 0.5],   // D5
            [11.5, 76, 0.8, 0.5], // E5
            [12, 79, 0.9, 1],     // G5
            [13, 76, 0.8, 1],     // E5
            [14, 72, 0.8, 1.75]   // C5
        ],
        bass: [
            [0, 48, 0.7, 1],     // C3
            [1, 48, 0.5, 1],
            [2, 48, 0.7, 1],
            [3, 52, 0.5, 1],     // E3
            [4, 45, 0.7, 1],     // A2
            [5, 45, 0.5, 1],
            [6, 48, 0.7, 1],     // C3
            [7, 43, 0.5, 1],     // G2
            [8, 50, 0.7, 1],     // D3
            [9, 50, 0.5, 1],
            [10, 48, 0.7, 1],    // C3
            [11, 48, 0.5, 1],
            [12, 43, 0.7, 1],    // G2
            [13, 43, 0.5, 1],
            [14, 48, 0.7, 1.75]  // C3
        ],
        perc: [
            [0, 36, 0.8, 0.25],
            [2, 36, 0.6, 0.25],
            [4, 36, 0.8, 0.25],
            [6, 36, 0.6, 0.25],
            [8, 36, 0.8, 0.25],
            [10, 36, 0.6, 0.25],
            [12, 36, 0.8, 0.25],
            [14, 36, 0.6, 0.25],
            [0.5, 42, 0.4, 0.1],
            [1.5, 42, 0.4, 0.1],
            [2.5, 42, 0.4, 0.1],
            [3.5, 42, 0.4, 0.1],
            [4.5, 42, 0.4, 0.1],
            [5.5, 42, 0.4, 0.1],
            [6.5, 42, 0.4, 0.1],
            [7.5, 42, 0.4, 0.1]
        ]
    },
    // Song C: "Neon Rush" - D minor, intense and driving for high levels
    {
        name: "Neon Rush", baseBPM: 155, loopBeats: 16,
        melodyWave: "square",
        bassWave: "sawtooth",
        melody: [
            [0, 74, 0.9, 0.5],    // D5
            [0.5, 74, 0.8, 0.5],  // D5
            [1, 77, 0.9, 0.5],    // F5
            [1.5, 74, 0.8, 0.5],  // D5
            [2, 72, 0.9, 0.5],    // C5
            [2.5, 69, 0.8, 0.5],  // A4
            [3, 72, 0.9, 1],      // C5
            [4, 74, 0.9, 0.5],    // D5
            [4.5, 77, 0.9, 0.5],  // F5
            [5, 79, 0.9, 0.5],    // G5
            [5.5, 77, 0.8, 0.5],  // F5
            [6, 74, 0.9, 1],      // D5
            [7, 72, 0.8, 0.5],    // C5
            [7.5, 69, 0.7, 0.5],  // A4
            [8, 70, 0.9, 0.5],    // Bb4
            [8.5, 70, 0.8, 0.5],  // Bb4
            [9, 72, 0.9, 0.5],    // C5
            [9.5, 74, 0.9, 0.5],  // D5
            [10, 77, 0.9, 1],     // F5
            [11, 79, 0.9, 0.5],   // G5
            [11.5, 77, 0.8, 0.5], // F5
            [12, 74, 0.9, 1],     // D5
            [13, 69, 0.8, 0.5],   // A4
            [13.5, 72, 0.8, 0.5], // C5
            [14, 74, 0.9, 1.75]   // D5
        ],
        bass: [
            [0, 38, 0.8, 0.5],   // D2
            [0.5, 38, 0.6, 0.5],
            [1, 38, 0.8, 0.5],
            [1.5, 38, 0.6, 0.5],
            [2, 36, 0.8, 0.5],   // C2
            [2.5, 36, 0.6, 0.5],
            [3, 36, 0.8, 1],
            [4, 38, 0.8, 0.5],   // D2
            [4.5, 38, 0.6, 0.5],
            [5, 41, 0.8, 0.5],   // F2
            [5.5, 41, 0.6, 0.5],
            [6, 38, 0.8, 1],     // D2
            [7, 36, 0.8, 1],     // C2
            [8, 34, 0.8, 0.5],   // Bb1
            [8.5, 34, 0.6, 0.5],
            [9, 36, 0.8, 0.5],   // C2
            [9.5, 36, 0.6, 0.5],
            [10, 41, 0.8, 1],    // F2
            [11, 43, 0.8, 1],    // G2
            [12, 38, 0.8, 1],    // D2
            [13, 33, 0.8, 1],    // A1
            [14, 38, 0.8, 1.75]  // D2
        ],
        perc: [
            [0, 36, 0.9, 0.2],
            [1, 36, 0.7, 0.2],
            [2, 36, 0.9, 0.2],
            [3, 36, 0.7, 0.2],
            [4, 36, 0.9, 0.2],
            [5, 36, 0.7, 0.2],
            [6, 36, 0.9, 0.2],
            [7, 36, 0.7, 0.2],
            [8, 36, 0.9, 0.2],
            [9, 36, 0.7, 0.2],
            [10, 36, 0.9, 0.2],
            [11, 36, 0.7, 0.2],
            [12, 36, 0.9, 0.2],
            [13, 36, 0.7, 0.2],
            [14, 36, 0.9, 0.2],
            [15, 36, 0.7, 0.2]
        ]
    }
];

function initAudio() {
    try { audioCtx = new AudioContext(); } catch(e) { audioCtx = null; return; }

    // Create mix buses: one for music, one for SFX
    try {
        musicBus = audioCtx.createBus();
        sfxBus = audioCtx.createBus();
        audioCtx.setBusGain(musicBus, settings.musicVol / 100);
        audioCtx.setBusGain(sfxBus, settings.sfxVol / 100);

        // Music bus effects: reverb for depth
        audioCtx.setBusReverbEnabled(musicBus, true);
        audioCtx.setBusReverbRoomSize(musicBus, 0.3);
        audioCtx.setBusReverbDamping(musicBus, 0.6);
        audioCtx.setBusReverbMix(musicBus, 0.15);

        // Subtle chorus on music
        audioCtx.setBusChorusEnabled(musicBus, true);
        audioCtx.setBusChorusRate(musicBus, 0.8);
        audioCtx.setBusChorusDepth(musicBus, 0.3);
        audioCtx.setBusChorusMix(musicBus, 0.1);

        // Master compressor for polish
        audioCtx.setCompressorEnabled(true);
        audioCtx.setCompressorThreshold(-12);
        audioCtx.setCompressorRatio(3);
        audioCtx.setCompressorAttack(0.01);
        audioCtx.setCompressorRelease(0.1);
    } catch(e) {
        musicBus = -1;
        sfxBus = -1;
    }
}

// --- SFX (direct voice API — OscillatorNode.stop() leaks voices) ---
function playTone(freq, duration, type, vol) {
    if (!audioCtx) return;
    var v = (vol !== undefined ? vol : 1.0) * (settings.sfxVol / 100);
    if (v <= 0) return;
    try {
        var voiceId = audioCtx.createVoice();
        audioCtx.setVoiceWaveform(voiceId, type || "square");
        audioCtx.setVoiceFrequency(voiceId, freq);
        audioCtx.setVoiceGain(voiceId, v * 15.0);
        audioCtx.setVoiceAttack(voiceId, 0.003);
        audioCtx.setVoiceDecay(voiceId, duration * 0.8);
        audioCtx.setVoiceSustain(voiceId, 0.0);
        audioCtx.setVoiceRelease(voiceId, 0.02);
        var t = audioCtx.currentTime;
        audioCtx.startVoice(voiceId, t);
        audioCtx.stopVoice(voiceId, t + duration);
    } catch(e) {}
}

function sfxMove()     { playTone(200, 0.05, "square", 0.4); }
function sfxRotate()   { playTone(300, 0.06, "square", 0.5); }
function sfxDrop()     { playTone(120, 0.12, "triangle", 0.8); }
function sfxLock()     { playTone(160, 0.08, "triangle", 0.5); }
function sfxHold()     { playTone(250, 0.06, "sine", 0.4); }
function sfxClear1()   { playTone(523, 0.15, "square", 0.6); }
function sfxClear2()   { playTone(659, 0.15, "square", 0.7); }
function sfxClear3()   { playTone(784, 0.18, "square", 0.8); }
function sfxTetris() {
    playTone(523, 0.1, "square", 0.8);
    setTimeout(function() { playTone(659, 0.1, "square", 0.8); }, 80);
    setTimeout(function() { playTone(784, 0.12, "square", 0.9); }, 160);
    setTimeout(function() { playTone(1047, 0.2, "square", 1.0); }, 240);
}
function sfxLevelUp() {
    playTone(440, 0.08, "sine", 0.6);
    setTimeout(function() { playTone(554, 0.08, "sine", 0.7); }, 80);
    setTimeout(function() { playTone(659, 0.12, "sine", 0.8); }, 160);
}
function sfxGameOver() {
    playTone(300, 0.2, "sawtooth", 0.5);
    setTimeout(function() { playTone(250, 0.2, "sawtooth", 0.5); }, 200);
    setTimeout(function() { playTone(200, 0.4, "sawtooth", 0.5); }, 400);
}
function sfxMenuMove() { playTone(400, 0.03, "sine", 0.3); }
function sfxMenuSelect() { playTone(600, 0.08, "square", 0.4); }
function sfxCombo(n) {
    var f = 400 + n * 80;
    if (f > 1200) f = 1200;
    playTone(f, 0.1, "square", 0.6);
}

// --- Music system (broaudio Sequence + VoiceAllocator) ---
function getSongForLevel(lvl) {
    if (lvl <= 7) return 0;        // Block March
    if (lvl <= 14) return 1;       // Crystal Stack
    return 2;                       // Neon Rush
}

function getMusicBPM(songIdx, lvl) {
    var song = SONGS[songIdx];
    return song.baseBPM + (lvl - 1) * 4;
}

function midiToHz(note) {
    return 440 * Math.pow(2, (note - 69) / 12);
}

function buildSequences(songIdx) {
    if (!audioCtx) return;
    var song = SONGS[songIdx];

    // Clean up existing sequences
    stopMusic();

    // Create melody voice allocator
    melodyAlloc = audioCtx.createVoiceAllocator(8);
    melodyAlloc.setStealPolicy("oldest");
    melodyAlloc.setVoiceSetup(function(voiceId, note, velocity) {
        var freq = 440 * Math.pow(2, (note - 69) / 12);
        audioCtx.setVoiceNote(voiceId, note, velocity);
        audioCtx.setVoiceWaveform(voiceId, song.melodyWave);
        audioCtx.setVoiceFrequency(voiceId, freq);
        audioCtx.setVoiceGain(voiceId, 15.0);
        audioCtx.setVoicePan(voiceId, 0);
        audioCtx.setVoiceAttack(voiceId, 0.008);
        audioCtx.setVoiceDecay(voiceId, 0.08);
        audioCtx.setVoiceSustain(voiceId, 0.7);
        audioCtx.setVoiceRelease(voiceId, 0.06);
        if (musicBus !== -1) {
            audioCtx.setVoiceBus(voiceId, musicBus);
        }
    });

    // Create bass voice allocator
    bassAlloc = audioCtx.createVoiceAllocator(4);
    bassAlloc.setStealPolicy("oldest");
    bassAlloc.setVoiceSetup(function(voiceId, note, velocity) {
        var freq = 440 * Math.pow(2, (note - 69) / 12);
        audioCtx.setVoiceNote(voiceId, note, velocity);
        audioCtx.setVoiceWaveform(voiceId, song.bassWave);
        audioCtx.setVoiceFrequency(voiceId, freq);
        audioCtx.setVoiceGain(voiceId, 15.0);
        audioCtx.setVoicePan(voiceId, 0);
        audioCtx.setVoiceAttack(voiceId, 0.01);
        audioCtx.setVoiceDecay(voiceId, 0.1);
        audioCtx.setVoiceSustain(voiceId, 0.8);
        audioCtx.setVoiceRelease(voiceId, 0.08);
        if (musicBus !== -1) {
            audioCtx.setVoiceBus(voiceId, musicBus);
        }
    });

    // Create percussion voice allocator
    percAlloc = audioCtx.createVoiceAllocator(4);
    percAlloc.setStealPolicy("oldest");
    percAlloc.setVoiceSetup(function(voiceId, note, velocity) {
        audioCtx.setVoiceNote(voiceId, note, velocity);
        if (note >= 40) {
            // Hi-hat: white noise, very short
            audioCtx.setVoiceWaveform(voiceId, "whitenoise");
            audioCtx.setVoiceGain(voiceId, 10.0);
            audioCtx.setVoiceAttack(voiceId, 0.001);
            audioCtx.setVoiceDecay(voiceId, 0.04);
            audioCtx.setVoiceSustain(voiceId, 0.0);
            audioCtx.setVoiceRelease(voiceId, 0.02);
            audioCtx.setVoiceFilterEnabled(voiceId, true);
            audioCtx.setVoiceFilterType(voiceId, "highpass");
            audioCtx.setVoiceFilterFrequency(voiceId, 8000);
        } else {
            // Kick: triangle low freq, punchy
            audioCtx.setVoiceWaveform(voiceId, "triangle");
            audioCtx.setVoiceFrequency(voiceId, 55);
            audioCtx.setVoiceGain(voiceId, 15.0);
            audioCtx.setVoiceAttack(voiceId, 0.002);
            audioCtx.setVoiceDecay(voiceId, 0.12);
            audioCtx.setVoiceSustain(voiceId, 0.0);
            audioCtx.setVoiceRelease(voiceId, 0.05);
        }
        if (musicBus !== -1) {
            audioCtx.setVoiceBus(voiceId, musicBus);
        }
    });

    // Build melody sequence
    melodySeq = audioCtx.createSequence(melodyAlloc);
    for (var i = 0; i < song.melody.length; i++) {
        var n = song.melody[i];
        melodySeq.addNote(n[0], n[1], n[2], n[3]);
    }
    melodySeq.setLoopEnabled(true);
    melodySeq.setLoopRange(0, song.loopBeats);

    // Build bass sequence
    bassSeq = audioCtx.createSequence(bassAlloc);
    for (var i = 0; i < song.bass.length; i++) {
        var n = song.bass[i];
        bassSeq.addNote(n[0], n[1], n[2], n[3]);
    }
    bassSeq.setLoopEnabled(true);
    bassSeq.setLoopRange(0, song.loopBeats);

    // Build percussion sequence
    percSeq = audioCtx.createSequence(percAlloc);
    for (var i = 0; i < song.perc.length; i++) {
        var n = song.perc[i];
        percSeq.addNote(n[0], n[1], n[2], n[3]);
    }
    percSeq.setLoopEnabled(true);
    percSeq.setLoopRange(0, song.loopBeats);

    currentSongIndex = songIdx;
}

function startMusic() {
    if (!audioCtx || !melodySeq) return;
    var bpm = getMusicBPM(currentSongIndex, level);
    melodySeq.setBPM(bpm);
    bassSeq.setBPM(bpm);
    percSeq.setBPM(bpm);
    var t = audioCtx.currentTime;
    melodySeq.play(t);
    bassSeq.play(t);
    percSeq.play(t);
    musicPlaying = true;
}

function stopMusic() {
    musicPlaying = false;
    if (melodySeq) { try { melodySeq.stop(); } catch(e) {} }
    if (bassSeq) { try { bassSeq.stop(); } catch(e) {} }
    if (percSeq) { try { percSeq.stop(); } catch(e) {} }
    if (melodyAlloc) { try { melodyAlloc.allNotesOff(); } catch(e) {} }
    if (bassAlloc) { try { bassAlloc.allNotesOff(); } catch(e) {} }
    if (percAlloc) { try { percAlloc.allNotesOff(); } catch(e) {} }
    melodySeq = null;
    bassSeq = null;
    percSeq = null;
    melodyAlloc = null;
    bassAlloc = null;
    percAlloc = null;
    currentSongIndex = -1;
}

function pauseMusic() {
    if (!musicPlaying) return;
    var t = audioCtx.currentTime;
    if (melodySeq) { try { melodySeq.pause(t); } catch(e) {} }
    if (bassSeq) { try { bassSeq.pause(t); } catch(e) {} }
    if (percSeq) { try { percSeq.pause(t); } catch(e) {} }
    if (melodyAlloc) { try { melodyAlloc.allNotesOff(); } catch(e) {} }
    if (bassAlloc) { try { bassAlloc.allNotesOff(); } catch(e) {} }
    if (percAlloc) { try { percAlloc.allNotesOff(); } catch(e) {} }
}

function resumeMusic() {
    if (!musicPlaying) return;
    var t = audioCtx.currentTime;
    if (melodySeq) { try { melodySeq.resume(t); } catch(e) {} }
    if (bassSeq) { try { bassSeq.resume(t); } catch(e) {} }
    if (percSeq) { try { percSeq.resume(t); } catch(e) {} }
}

function updateMusicBPM() {
    if (!musicPlaying || !melodySeq) return;
    var bpm = getMusicBPM(currentSongIndex, level);
    melodySeq.setBPM(bpm);
    bassSeq.setBPM(bpm);
    percSeq.setBPM(bpm);
}

function updateMusicVolume() {
    if (!audioCtx || musicBus === -1) return;
    try { audioCtx.setBusGain(musicBus, settings.musicVol / 100); } catch(e) {}
}

function updateSfxVolume() {
    if (!audioCtx || sfxBus === -1) return;
    try { audioCtx.setBusGain(sfxBus, settings.sfxVol / 100); } catch(e) {}
}

function checkSongChange() {
    var wantSong = getSongForLevel(level);
    if (wantSong !== currentSongIndex) {
        buildSequences(wantSong);
        startMusic();
    }
}

function updateSequences() {
    if (!musicPlaying || !audioCtx) return;
    var t = audioCtx.currentTime;
    if (melodySeq) { try { melodySeq.update(t); } catch(e) {} }
    if (bassSeq) { try { bassSeq.update(t); } catch(e) {} }
    if (percSeq) { try { percSeq.update(t); } catch(e) {} }
}

// --- Game state ---
var board = [];
// Initialize board immediately
(function() {
    for (var i = 0; i < ROWS; i++) {
        board[i] = [];
        for (var j = 0; j < COLS; j++) board[i][j] = 0;
    }
})();
var cur = null; // {type, x, y, rot}
var nextTypes = []; // upcoming pieces (bag)
var holdType = 0;
var holdUsed = false;
var score = 0, level = 1, totalLines = 0;
var combo = -1;
var backToBack = false;
var gameState = STATE_MENU;
var gameTime = 0;
var piecesPlaced = 0;
var lastFrameTime = 0;

// Layout
var CELL = 0, BOARD_W = 0, BOARD_H = 0, BOARD_X = 0, BOARD_Y = 0;

function calcLayout() {
    var W = getW(), H = getH();
    CELL = Math.floor(Math.min(H / (ROWS + 4), W / (COLS + 10)));
    BOARD_W = COLS * CELL;
    BOARD_H = ROWS * CELL;
    BOARD_X = Math.floor((W - BOARD_W) / 2);
    BOARD_Y = Math.floor((H - BOARD_H) / 2);
}

// Timing
var dropTimer = 0;
var dropInterval = 0;
var lockTimer = 0;
var lockDelay = 500; // ms before piece locks
var lockMoves = 0;
var maxLockMoves = 15;
var softDropping = false;

// DAS (Delayed Auto Shift)
var dasDelay = 167; // ms
var arrDelay = 33; // ms (auto repeat rate)
var dasTimer = 0;
var dasDir = 0; // -1 left, 1 right, 0 none
var dasActive = false;
var dasKey = "";

// Soft drop repeat
var softDropTimer = 0;
var softDropRate = 30; // ms between soft drops

// Keys currently held
var keysDown = {};

// Animation state
var lineClearAnim = null; // {rows:[], timer:0, duration:300}
var shakeTimer = 0;
var shakeMag = 0;
var particles = [];
var actionTextTimer = 0;
var actionTextStr = "";
var flashCells = []; // [{r,c,timer,color}]

// --- 7-bag randomizer ---
var bag = [];
function refillBag() {
    bag = [1, 2, 3, 4, 5, 6, 7];
    // Fisher-Yates shuffle
    for (var i = bag.length - 1; i > 0; i--) {
        var j = Math.floor(Math.random() * (i + 1));
        var tmp = bag[i]; bag[i] = bag[j]; bag[j] = tmp;
    }
}
function nextPiece() {
    if (bag.length === 0) refillBag();
    return bag.pop();
}
function ensureNextTypes() {
    while (nextTypes.length < 5) {
        nextTypes.push(nextPiece());
    }
}

// --- Board ---
function resetBoard() {
    board = [];
    for (var i = 0; i < ROWS; i++) {
        board[i] = [];
        for (var j = 0; j < COLS; j++) board[i][j] = 0;
    }
}

function canPlace(type, x, y, rot) {
    var cells = PIECES[type][rot & 3];
    for (var i = 0; i < cells.length; i++) {
        var r = y + cells[i][0];
        var c = x + cells[i][1];
        if (c < 0 || c >= COLS || r >= ROWS) return false;
        if (r >= 0 && board[r][c] !== 0) return false;
    }
    return true;
}

function ghostY() {
    if (!cur) return 0;
    var gy = cur.y;
    while (canPlace(cur.type, cur.x, gy + 1, cur.rot)) gy++;
    return gy;
}

// --- Spawn ---
function spawnPiece() {
    ensureNextTypes();
    var type = nextTypes.shift();
    ensureNextTypes();
    cur = { type: type, x: 3, y: -1, rot: 0 };
    holdUsed = false;
    dropTimer = 0;
    lockTimer = 0;
    lockMoves = 0;
    softDropping = false;
    if (!canPlace(cur.type, cur.x, cur.y, cur.rot)) {
        // Try one row up
        cur.y = -2;
        if (!canPlace(cur.type, cur.x, cur.y, cur.rot)) {
            gameOver();
        }
    }
}

// --- Lock ---
function lockPiece() {
    if (!cur) return;
    var cells = PIECES[cur.type][cur.rot & 3];
    for (var i = 0; i < cells.length; i++) {
        var r = cur.y + cells[i][0];
        var c = cur.x + cells[i][1];
        if (r >= 0 && r < ROWS && c >= 0 && c < COLS) {
            board[r][c] = cur.type;
            flashCells.push({r: r, c: c, timer: 200, color: COLORS_LIGHT[cur.type]});
        }
    }
    piecesPlaced++;
    sfxLock();
    clearLines();
    spawnPiece();
}

// --- Line clearing ---
function clearLines() {
    var cleared = [];
    for (var r = ROWS - 1; r >= 0; r--) {
        var full = true;
        for (var c = 0; c < COLS; c++) {
            if (board[r][c] === 0) { full = false; break; }
        }
        if (full) cleared.push(r);
    }

    if (cleared.length === 0) {
        combo = -1;
        return;
    }

    combo++;

    // Scoring
    var pts = [0, 100, 300, 500, 800];
    var baseScore = pts[cleared.length] * level;

    // T-spin detection (simplified: T piece, rotation was last move, 3+ corners filled)
    var isTSpin = false;
    // Skipping full T-spin detection for simplicity — just use line count

    // Back-to-back bonus
    var isTetris = (cleared.length === 4);
    if (isTetris || isTSpin) {
        if (backToBack) baseScore = Math.floor(baseScore * 1.5);
        backToBack = true;
    } else {
        backToBack = false;
    }

    // Combo bonus
    if (combo > 0) {
        baseScore += 50 * combo * level;
        sfxCombo(combo);
    }

    score += baseScore;
    totalLines += cleared.length;

    // Level up
    var newLevel = Math.floor(totalLines / 10) + settings.startLevel;
    if (newLevel !== level) {
        level = newLevel;
        sfxLevelUp();
        showActionText("LEVEL " + level);
        checkSongChange();
        updateMusicBPM();
    }

    // Sound effects
    if (cleared.length === 4) {
        sfxTetris();
        showActionText("TETRIS!");
        shakeTimer = 300;
        shakeMag = 8;
    } else if (cleared.length === 3) {
        sfxClear3();
        showActionText("TRIPLE");
    } else if (cleared.length === 2) {
        sfxClear2();
        showActionText("DOUBLE");
    } else {
        sfxClear1();
    }

    if (combo > 1) {
        showActionText(combo + "x COMBO!");
    }

    // Start line clear animation
    lineClearAnim = { rows: cleared, timer: 0, duration: 250 };

    // Spawn particles along cleared rows
    for (var ri = 0; ri < cleared.length; ri++) {
        var row = cleared[ri];
        for (var c = 0; c < COLS; c++) {
            var color = COLORS[board[row][c]] || "#fff";
            for (var p = 0; p < 3; p++) {
                particles.push({
                    x: BOARD_X + c * CELL + CELL / 2,
                    y: BOARD_Y + row * CELL + CELL / 2,
                    vx: (Math.random() - 0.5) * 4,
                    vy: (Math.random() - 1) * 3,
                    life: 400 + Math.random() * 300,
                    maxLife: 400 + Math.random() * 300,
                    size: 2 + Math.random() * 3,
                    color: color
                });
            }
        }
    }

    // Actually remove the lines (after animation starts)
    // We remove immediately and let animation handle visuals
    for (var ri = 0; ri < cleared.length; ri++) {
        board.splice(cleared[ri], 1);
        var emptyRow = [];
        for (var c = 0; c < COLS; c++) emptyRow.push(0);
        board.unshift(emptyRow);
        // Adjust subsequent indices
        for (var rj = ri + 1; rj < cleared.length; rj++) {
            if (cleared[rj] < cleared[ri]) cleared[rj]++;
        }
    }

    updateHUD();
}

// --- Movement ---
function moveLeft() {
    if (!cur) return false;
    if (canPlace(cur.type, cur.x - 1, cur.y, cur.rot)) {
        cur.x--;
        resetLockTimer();
        sfxMove();
        return true;
    }
    return false;
}

function moveRight() {
    if (!cur) return false;
    if (canPlace(cur.type, cur.x + 1, cur.y, cur.rot)) {
        cur.x++;
        resetLockTimer();
        sfxMove();
        return true;
    }
    return false;
}

function moveDown() {
    if (!cur) return false;
    if (canPlace(cur.type, cur.x, cur.y + 1, cur.rot)) {
        cur.y++;
        return true;
    }
    return false;
}

function rotateCW() {
    if (!cur) return;
    tryRotate((cur.rot + 1) & 3);
}

function rotateCCW() {
    if (!cur) return;
    tryRotate((cur.rot + 3) & 3);
}

function tryRotate(newRot) {
    if (!cur) return;
    var kickData = (cur.type === 1) ? KICKS.I : KICKS.normal;
    var kickSet = kickData[cur.rot];

    for (var i = 0; i < kickSet.length; i++) {
        var dx = kickSet[i][0];
        var dy = -kickSet[i][1]; // SRS uses Y-up, our board uses Y-down
        if (canPlace(cur.type, cur.x + dx, cur.y + dy, newRot)) {
            cur.x += dx;
            cur.y += dy;
            cur.rot = newRot;
            resetLockTimer();
            sfxRotate();
            return;
        }
    }
}

function hardDrop() {
    if (!cur) return;
    var gy = ghostY();
    var dropDist = gy - cur.y;
    score += dropDist * 2;

    // Trail effect
    var cells = PIECES[cur.type][cur.rot & 3];
    for (var i = 0; i < cells.length; i++) {
        var c = cur.x + cells[i][1];
        for (var r = cur.y + cells[i][0]; r <= gy + cells[i][0]; r++) {
            if (r >= 0 && r < ROWS) {
                flashCells.push({r: r, c: c, timer: 120, color: COLORS[cur.type]});
            }
        }
    }

    cur.y = gy;
    sfxDrop();
    lockPiece();
    updateHUD();
}

function doHold() {
    if (!cur || holdUsed) return;
    sfxHold();
    var type = cur.type;
    if (holdType === 0) {
        holdType = type;
        spawnPiece();
    } else {
        var tmp = holdType;
        holdType = type;
        cur = { type: tmp, x: 3, y: -1, rot: 0 };
        dropTimer = 0;
        lockTimer = 0;
        lockMoves = 0;
    }
    holdUsed = true;
}

function resetLockTimer() {
    if (lockMoves < maxLockMoves) {
        lockTimer = 0;
        lockMoves++;
    }
}

// --- Speed ---
function getDropInterval() {
    var speeds = [800,717,633,550,467,383,300,217,133,100,83,83,83,67,67,67,50,50,50,33];
    var idx = level - 1;
    if (idx < 0) idx = 0;
    if (idx >= speeds.length) idx = speeds.length - 1;
    return speeds[idx];
}

// --- Game lifecycle ---
function startGame() {
    resetBoard();
    score = 0;
    level = settings.startLevel;
    totalLines = 0;
    combo = -1;
    backToBack = false;
    holdType = 0;
    holdUsed = false;
    gameTime = 0;
    piecesPlaced = 0;
    bag = [];
    nextTypes = [];
    particles = [];
    flashCells = [];
    lineClearAnim = null;
    shakeTimer = 0;
    actionTextTimer = 0;
    keysDown = {};
    dasDir = 0;
    dasTimer = 0;
    dasActive = false;

    refillBag();
    ensureNextTypes();
    spawnPiece();
    gameState = STATE_PLAYING;
    dropInterval = getDropInterval();

    overlayEl.style.display = "none";
    updateHUD();

    // Start music for current level
    var songIdx = getSongForLevel(level);
    buildSequences(songIdx);
    startMusic();
}

function gameOver() {
    gameState = STATE_GAMEOVER;
    cur = null;
    stopMusic();
    sfxGameOver();

    var statsText = "Score: " + score + "  Level: " + level +
                    "  Lines: " + totalLines + "  Pieces: " + piecesPlaced;
    gameoverStatsEl.textContent = statsText;
    showMenu("menu-gameover");
}

function pauseGame() {
    if (gameState === STATE_PLAYING) {
        gameState = STATE_PAUSED;
        pauseMusic();
        showMenu("menu-pause");
    }
}

function resumeGame() {
    if (gameState === STATE_PAUSED) {
        gameState = STATE_PLAYING;
        overlayEl.style.display = "none";
        keysDown = {};
        dasDir = 0;
        resumeMusic();
    }
}

// --- Action text ---
function showActionText(text) {
    actionTextStr = text;
    actionTextTimer = 800;
}

// --- HUD ---
var scoreEl = document.getElementById("score");
var levelEl = document.getElementById("level");
var linesEl = document.getElementById("lines");
var comboEl = document.getElementById("combo");
var overlayEl = document.getElementById("overlay");
var gameoverStatsEl = document.getElementById("gameover-stats");
var actionTextEl = document.getElementById("action-text");

function updateHUD() {
    scoreEl.textContent = String(score);
    levelEl.textContent = String(level);
    linesEl.textContent = String(totalLines);
    comboEl.textContent = combo > 0 ? String(combo) : "0";
}

// --- Menu system ---
var currentMenu = "menu-main";
var menuIndex = 0;
var rebinding = false;
var rebindAction = "";

function showMenu(menuId) {
    // Hide all
    var menus = ["menu-main", "menu-settings", "menu-controls", "menu-pause", "menu-gameover"];
    for (var i = 0; i < menus.length; i++) {
        var el = document.getElementById(menus[i]);
        if (el) el.style.display = "none";
    }
    var el = document.getElementById(menuId);
    if (el) el.style.display = "";
    overlayEl.style.display = "";
    currentMenu = menuId;
    menuIndex = 0;
    rebinding = false;

    // Hide rebind prompt whenever entering controls menu
    var prompt = document.getElementById("rebind-prompt");
    if (prompt) prompt.style.display = "none";

    if (menuId === "menu-controls") buildControlsList();
    if (menuId === "menu-settings") updateSettingsDisplay();

    updateMenuSelection();
}

function getMenuItems() {
    var el = document.getElementById(currentMenu);
    if (!el) return [];
    var items = [];
    var children = el.children;
    for (var i = 0; i < children.length; i++) {
        // Check the menu-items container
        if (children[i].className === "menu-items") {
            var sub = children[i].children;
            for (var j = 0; j < sub.length; j++) {
                if (sub[j].className.indexOf("menu-item") !== -1) {
                    items.push(sub[j]);
                }
            }
        }
    }
    return items;
}

function updateMenuSelection() {
    var items = getMenuItems();
    for (var i = 0; i < items.length; i++) {
        if (i === menuIndex) {
            items[i].className = "menu-item selected";
        } else {
            items[i].className = "menu-item";
        }
    }
}

function menuUp() {
    var items = getMenuItems();
    if (items.length === 0) return;
    menuIndex--;
    if (menuIndex < 0) menuIndex = items.length - 1;
    sfxMenuMove();
    updateMenuSelection();
}

function menuDown() {
    var items = getMenuItems();
    if (items.length === 0) return;
    menuIndex++;
    if (menuIndex >= items.length) menuIndex = 0;
    sfxMenuMove();
    updateMenuSelection();
}

function activateItem(item) {
    if (!item) return;
    var action = item.getAttribute("data-action");
    var setting = item.getAttribute("data-setting");

    sfxMenuSelect();

    if (action === "start" || action === "restart") {
        startGame();
    } else if (action === "settings") {
        showMenu("menu-settings");
    } else if (action === "controls") {
        showMenu("menu-controls");
    } else if (action === "back" || action === "quit") {
        showMenu("menu-main");
        if (gameState === STATE_PAUSED) gameState = STATE_MENU;
    } else if (action === "resume") {
        resumeGame();
    } else if (action === "rebind") {
        var controlName = item.getAttribute("data-control");
        if (controlName) {
            rebinding = true;
            rebindAction = controlName;
            var prompt = document.getElementById("rebind-prompt");
            if (prompt) prompt.style.display = "";
        }
    } else if (action === "resetControls") {
        controls = JSON.parse(JSON.stringify(DEFAULT_CONTROLS));
        saveSettings();
        buildControlsList();
        updateMenuSelection();
    } else if (setting) {
        // Toggle/cycle settings on click
        menuAdjust(1);
    }
}

function menuSelect() {
    var items = getMenuItems();
    if (menuIndex >= items.length) return;
    activateItem(items[menuIndex]);
}

function menuAdjust(dir) {
    var items = getMenuItems();
    if (menuIndex >= items.length) return;
    var item = items[menuIndex];
    var setting = item.getAttribute("data-setting");
    if (!setting) return;

    if (setting === "startLevel") {
        settings.startLevel += dir;
        if (settings.startLevel < 1) settings.startLevel = 1;
        if (settings.startLevel > 20) settings.startLevel = 20;
    } else if (setting === "sfxVol") {
        settings.sfxVol += dir * 10;
        if (settings.sfxVol < 0) settings.sfxVol = 0;
        if (settings.sfxVol > 100) settings.sfxVol = 100;
        updateSfxVolume();
    } else if (setting === "musicVol") {
        settings.musicVol += dir * 10;
        if (settings.musicVol < 0) settings.musicVol = 0;
        if (settings.musicVol > 100) settings.musicVol = 100;
        updateMusicVolume();
    } else if (setting === "ghostPiece") {
        settings.ghostPiece = !settings.ghostPiece;
    } else if (setting === "gridLines") {
        settings.gridLines = !settings.gridLines;
    }

    saveSettings();
    updateSettingsDisplay();
    sfxMenuMove();
}

function updateSettingsDisplay() {
    var el;
    el = document.getElementById("opt-startLevel");
    if (el) el.textContent = String(settings.startLevel);
    el = document.getElementById("opt-sfxVol");
    if (el) el.textContent = String(settings.sfxVol);
    el = document.getElementById("opt-musicVol");
    if (el) el.textContent = String(settings.musicVol);
    el = document.getElementById("opt-ghostPiece");
    if (el) el.textContent = settings.ghostPiece ? "ON" : "OFF";
    el = document.getElementById("opt-gridLines");
    if (el) el.textContent = settings.gridLines ? "ON" : "OFF";
}

function buildControlsList() {
    for (var i = 0; i < CONTROL_NAMES.length; i++) {
        var name = CONTROL_NAMES[i];
        var label = CONTROL_LABELS[name];
        var key = controls[name] || "???";
        var displayKey = keyDisplayName(key);
        var el = document.getElementById("ctrl-" + name);
        if (el) el.textContent = label + ": " + displayKey;
    }
}

function keyDisplayName(key) {
    if (key === " ") return "Space";
    if (key === "ArrowLeft") return "\u2190";
    if (key === "ArrowRight") return "\u2192";
    if (key === "ArrowUp") return "\u2191";
    if (key === "ArrowDown") return "\u2193";
    if (key.length === 1) return key.toUpperCase();
    return key;
}

// --- Input handling ---
function getAction(key) {
    for (var name in controls) {
        if (controls[name] === key) return name;
    }
    return null;
}

// --- Mouse click support for menus ---
function setupMenuClicks() {
    var allMenuIds = ["menu-main", "menu-settings", "menu-controls", "menu-pause", "menu-gameover"];
    for (var m = 0; m < allMenuIds.length; m++) {
        var menuEl = document.getElementById(allMenuIds[m]);
        if (!menuEl) continue;
        var children = menuEl.children;
        for (var i = 0; i < children.length; i++) {
            if (children[i].className && children[i].className.indexOf("menu-items") !== -1) {
                var items = children[i].children;
                for (var j = 0; j < items.length; j++) {
                    (function(item, idx) {
                        item.addEventListener("click", function() {
                            if (rebinding) return;
                            // Set the menu index to this item and activate it
                            menuIndex = idx;
                            updateMenuSelection();
                            activateItem(item);
                        });
                    })(items[j], j);
                }
            }
        }
    }
}

document.body.addEventListener("keydown", function(e) {
    var key = e.key;

    // Rebinding mode
    if (rebinding) {
        if (key === "Escape") {
            rebinding = false;
            var prompt = document.getElementById("rebind-prompt");
            if (prompt) prompt.style.display = "none";
            return;
        }
        controls[rebindAction] = key;
        saveSettings();
        rebinding = false;
        var prompt = document.getElementById("rebind-prompt");
        if (prompt) prompt.style.display = "none";
        buildControlsList();
        updateMenuSelection();
        return;
    }

    // Menu navigation
    if (gameState === STATE_MENU || gameState === STATE_PAUSED || gameState === STATE_GAMEOVER) {
        if (key === "ArrowUp") { menuUp(); return; }
        if (key === "ArrowDown") { menuDown(); return; }
        if (key === "Enter") {
            menuSelect();
            return;
        }
        if (key === "ArrowLeft") { menuAdjust(-1); return; }
        if (key === "ArrowRight") { menuAdjust(1); return; }
        if (key === "Escape") {
            if (currentMenu === "menu-settings" || currentMenu === "menu-controls") {
                showMenu("menu-main");
            } else if (gameState === STATE_PAUSED) {
                resumeGame();
            }
            return;
        }
        return;
    }

    // Game input
    if (gameState !== STATE_PLAYING || !cur) return;
    if (e.repeat) {
        // We handle repeat ourselves via DAS
        return;
    }

    var action = getAction(key);
    if (!action) return;

    keysDown[action] = true;

    if (action === "moveLeft") {
        moveLeft();
        dasDir = -1;
        dasTimer = 0;
        dasActive = false;
        dasKey = action;
    } else if (action === "moveRight") {
        moveRight();
        dasDir = 1;
        dasTimer = 0;
        dasActive = false;
        dasKey = action;
    } else if (action === "softDrop") {
        softDropping = true;
        softDropTimer = 0;
        if (moveDown()) {
            score += 1;
            updateHUD();
        }
    } else if (action === "hardDrop") {
        hardDrop();
    } else if (action === "rotCW") {
        rotateCW();
    } else if (action === "rotCCW") {
        rotateCCW();
    } else if (action === "hold") {
        doHold();
    } else if (action === "pause") {
        pauseGame();
    }
});

document.body.addEventListener("keyup", function(e) {
    var key = e.key;
    var action = getAction(key);
    if (!action) return;

    keysDown[action] = false;

    if (action === "moveLeft" || action === "moveRight") {
        if (dasKey === action) {
            dasDir = 0;
            dasActive = false;
            // Check if opposite direction is still held
            if (action === "moveLeft" && keysDown["moveRight"]) {
                dasDir = 1;
                dasTimer = 0;
                dasKey = "moveRight";
            } else if (action === "moveRight" && keysDown["moveLeft"]) {
                dasDir = -1;
                dasTimer = 0;
                dasKey = "moveLeft";
            }
        }
    }
    if (action === "softDrop") {
        softDropping = false;
    }
});

// --- Update loop ---
function update(dt) {
    if (gameState !== STATE_PLAYING || !cur) return;

    gameTime += dt;

    // DAS handling
    if (dasDir !== 0) {
        dasTimer += dt;
        if (!dasActive) {
            if (dasTimer >= dasDelay) {
                dasActive = true;
                dasTimer = 0;
            }
        }
        if (dasActive) {
            dasTimer += 0; // already added above
            while (dasTimer >= arrDelay) {
                dasTimer -= arrDelay;
                if (dasDir === -1) moveLeft();
                else if (dasDir === 1) moveRight();
            }
        }
    }

    // Soft drop repeat
    if (softDropping) {
        softDropTimer += dt;
        while (softDropTimer >= softDropRate) {
            softDropTimer -= softDropRate;
            if (moveDown()) {
                score += 1;
            }
        }
        updateHUD();
    }

    // Gravity
    dropInterval = getDropInterval();
    dropTimer += dt;
    while (dropTimer >= dropInterval && !softDropping) {
        dropTimer -= dropInterval;
        moveDown();
    }

    // Lock delay — piece on ground
    if (cur && !canPlace(cur.type, cur.x, cur.y + 1, cur.rot)) {
        lockTimer += dt;
        if (lockTimer >= lockDelay) {
            lockPiece();
        }
    } else {
        lockTimer = 0;
    }

    // Animation updates
    if (lineClearAnim) {
        lineClearAnim.timer += dt;
        if (lineClearAnim.timer >= lineClearAnim.duration) {
            lineClearAnim = null;
        }
    }

    if (shakeTimer > 0) shakeTimer -= dt;
    if (actionTextTimer > 0) actionTextTimer -= dt;

    // Update particles
    for (var i = particles.length - 1; i >= 0; i--) {
        var p = particles[i];
        p.life -= dt;
        if (p.life <= 0) {
            particles.splice(i, 1);
            continue;
        }
        p.x += p.vx;
        p.y += p.vy;
        p.vy += 0.15; // gravity
    }

    // Flash cells decay
    for (var i = flashCells.length - 1; i >= 0; i--) {
        flashCells[i].timer -= dt;
        if (flashCells[i].timer <= 0) flashCells.splice(i, 1);
    }
}

// --- Drawing ---
function drawCell(col, row, color, alpha) {
    ctx.globalAlpha = alpha !== undefined ? alpha : 1.0;
    ctx.fillStyle = color;
    ctx.fillRect(BOARD_X + col * CELL + 1, BOARD_Y + row * CELL + 1, CELL - 2, CELL - 2);

    // Highlight (top-left edge)
    var light = COLORS_LIGHT[colorIndex(color)] || color;
    ctx.fillStyle = "rgba(255,255,255,0.15)";
    ctx.fillRect(BOARD_X + col * CELL + 1, BOARD_Y + row * CELL + 1, CELL - 2, 2);
    ctx.fillRect(BOARD_X + col * CELL + 1, BOARD_Y + row * CELL + 1, 2, CELL - 2);

    // Shadow (bottom-right edge)
    ctx.fillStyle = "rgba(0,0,0,0.2)";
    ctx.fillRect(BOARD_X + col * CELL + 1, BOARD_Y + (row + 1) * CELL - 3, CELL - 2, 2);
    ctx.fillRect(BOARD_X + (col + 1) * CELL - 3, BOARD_Y + row * CELL + 1, 2, CELL - 2);

    ctx.globalAlpha = 1.0;
}

function colorIndex(color) {
    for (var i = 1; i < COLORS.length; i++) {
        if (COLORS[i] === color) return i;
    }
    return 0;
}

function drawPiece(type, x, y, rot, alpha) {
    var cells = PIECES[type][rot & 3];
    for (var i = 0; i < cells.length; i++) {
        var r = y + cells[i][0];
        var c = x + cells[i][1];
        if (r >= 0) drawCell(c, r, COLORS[type], alpha);
    }
}

function drawMiniPiece(type, px, py, cellSize) {
    if (type <= 0) return;
    var cells = PIECES[type][0];
    // Center the piece
    var minC = 9, maxC = 0, minR = 9, maxR = 0;
    for (var i = 0; i < cells.length; i++) {
        if (cells[i][1] < minC) minC = cells[i][1];
        if (cells[i][1] > maxC) maxC = cells[i][1];
        if (cells[i][0] < minR) minR = cells[i][0];
        if (cells[i][0] > maxR) maxR = cells[i][0];
    }
    var pw = (maxC - minC + 1) * cellSize;
    var ph = (maxR - minR + 1) * cellSize;
    var ox = px + (cellSize * 4 - pw) / 2 - minC * cellSize;
    var oy = py + (cellSize * 3 - ph) / 2 - minR * cellSize;

    ctx.fillStyle = COLORS[type];
    for (var i = 0; i < cells.length; i++) {
        var cx = ox + cells[i][1] * cellSize;
        var cy = oy + cells[i][0] * cellSize;
        ctx.fillRect(cx + 1, cy + 1, cellSize - 2, cellSize - 2);
    }
}

function draw() {
    var W = getW(), H = getH();
    calcLayout();

    ctx.clearRect(0, 0, W, H);

    // Screen shake offset
    var shakeX = 0, shakeY = 0;
    if (shakeTimer > 0) {
        var intensity = (shakeTimer / 300) * shakeMag;
        shakeX = (Math.random() - 0.5) * intensity;
        shakeY = (Math.random() - 0.5) * intensity;
    }

    ctx.save();
    ctx.translate(shakeX, shakeY);

    // Board background
    ctx.fillStyle = "#08080e";
    ctx.fillRect(BOARD_X, BOARD_Y, BOARD_W, BOARD_H);

    // Grid lines
    if (settings.gridLines) {
        ctx.strokeStyle = "#181822";
        for (var c = 0; c <= COLS; c++) {
            var x = BOARD_X + c * CELL;
            ctx.strokeRect(x, BOARD_Y, 0, BOARD_H);
        }
        for (var r = 0; r <= ROWS; r++) {
            var y = BOARD_Y + r * CELL;
            ctx.strokeRect(BOARD_X, y, BOARD_W, 0);
        }
    }

    // Filled cells
    for (var r = 0; r < ROWS; r++) {
        if (!board[r]) continue;
        for (var c = 0; c < COLS; c++) {
            if (board[r][c] !== 0) {
                drawCell(c, r, COLORS[board[r][c]]);
            }
        }
    }

    // Flash cells (lock/drop animation)
    for (var i = 0; i < flashCells.length; i++) {
        var fc = flashCells[i];
        var alpha = fc.timer / 200;
        ctx.globalAlpha = alpha * 0.5;
        ctx.fillStyle = "#ffffff";
        ctx.fillRect(BOARD_X + fc.c * CELL, BOARD_Y + fc.r * CELL, CELL, CELL);
        ctx.globalAlpha = 1.0;
    }

    // Line clear animation flash
    if (lineClearAnim) {
        var progress = lineClearAnim.timer / lineClearAnim.duration;
        var flashAlpha = Math.sin(progress * Math.PI * 3) * 0.5;
        if (flashAlpha > 0) {
            ctx.globalAlpha = flashAlpha;
            ctx.fillStyle = "#ffffff";
            for (var i = 0; i < lineClearAnim.rows.length; i++) {
                // Rows have been removed already, so flash at top
                ctx.fillRect(BOARD_X, BOARD_Y, BOARD_W, CELL);
            }
            ctx.globalAlpha = 1.0;
        }
    }

    // Ghost piece
    if (cur && gameState === STATE_PLAYING && settings.ghostPiece) {
        var gy = ghostY();
        if (gy !== cur.y) {
            var cells = PIECES[cur.type][cur.rot & 3];
            for (var i = 0; i < cells.length; i++) {
                var r = gy + cells[i][0];
                var c = cur.x + cells[i][1];
                if (r >= 0) {
                    ctx.globalAlpha = 0.2;
                    ctx.fillStyle = COLORS[cur.type];
                    ctx.fillRect(BOARD_X + c * CELL + 1, BOARD_Y + r * CELL + 1,
                                 CELL - 2, CELL - 2);
                    ctx.globalAlpha = 0.4;
                    ctx.strokeStyle = COLORS[cur.type];
                    ctx.strokeRect(BOARD_X + c * CELL + 1, BOARD_Y + r * CELL + 1,
                                   CELL - 2, CELL - 2);
                    ctx.globalAlpha = 1.0;
                }
            }
        }
    }

    // Current piece (with lock delay visual — dims as lock approaches)
    if (cur && gameState === STATE_PLAYING) {
        var lockAlpha = 1.0;
        if (!canPlace(cur.type, cur.x, cur.y + 1, cur.rot) && lockTimer > 0) {
            lockAlpha = 1.0 - (lockTimer / lockDelay) * 0.3;
        }
        drawPiece(cur.type, cur.x, cur.y, cur.rot, lockAlpha);
    }

    // Board border
    ctx.strokeStyle = "#444";
    ctx.strokeRect(BOARD_X - 1, BOARD_Y - 1, BOARD_W + 2, BOARD_H + 2);

    // Hold piece preview (on canvas, left of board)
    var pvCellSize = Math.floor(CELL * 0.7);
    var holdX = BOARD_X - pvCellSize * 5 - 10;
    var holdY = BOARD_Y;
    ctx.fillStyle = "#0c0c14";
    ctx.fillRect(holdX, holdY, pvCellSize * 4 + 8, pvCellSize * 3 + 8);
    ctx.strokeStyle = "#333";
    ctx.strokeRect(holdX, holdY, pvCellSize * 4 + 8, pvCellSize * 3 + 8);
    ctx.fillStyle = "#666";
    ctx.font = "11px Consolas";
    ctx.fillText("HOLD", holdX + 4, holdY - 4);
    if (holdType > 0) {
        var holdAlpha = holdUsed ? 0.4 : 1.0;
        ctx.globalAlpha = holdAlpha;
        drawMiniPiece(holdType, holdX + 4, holdY + 4, pvCellSize);
        ctx.globalAlpha = 1.0;
    }

    // Next piece preview (right of board)
    var nextX = BOARD_X + BOARD_W + 10;
    var nextY = BOARD_Y;
    ctx.fillStyle = "#0c0c14";
    ctx.fillRect(nextX, nextY, pvCellSize * 4 + 8, pvCellSize * 3 + 8);
    ctx.strokeStyle = "#333";
    ctx.strokeRect(nextX, nextY, pvCellSize * 4 + 8, pvCellSize * 3 + 8);
    ctx.fillStyle = "#666";
    ctx.font = "11px Consolas";
    ctx.fillText("NEXT", nextX + 4, nextY - 4);
    if (nextTypes.length > 0) {
        drawMiniPiece(nextTypes[0], nextX + 4, nextY + 4, pvCellSize);
    }

    // Queue preview (smaller, below next)
    for (var qi = 1; qi < Math.min(nextTypes.length, 4); qi++) {
        var qy = nextY + (pvCellSize * 3 + 16) * qi + 8;
        ctx.fillStyle = "#0a0a10";
        ctx.fillRect(nextX, qy, pvCellSize * 4 + 8, pvCellSize * 3 + 8);
        ctx.strokeStyle = "#222";
        ctx.strokeRect(nextX, qy, pvCellSize * 4 + 8, pvCellSize * 3 + 8);
        ctx.globalAlpha = 0.6;
        drawMiniPiece(nextTypes[qi], nextX + 4, qy + 4, pvCellSize);
        ctx.globalAlpha = 1.0;
    }

    // Particles
    for (var i = 0; i < particles.length; i++) {
        var p = particles[i];
        var alpha = p.life / p.maxLife;
        ctx.globalAlpha = alpha;
        ctx.fillStyle = p.color;
        ctx.fillRect(p.x - p.size / 2, p.y - p.size / 2, p.size, p.size);
    }
    ctx.globalAlpha = 1.0;

    // Action text
    if (actionTextTimer > 0) {
        var alpha = Math.min(1.0, actionTextTimer / 200);
        var scale = 1.0 + (1.0 - Math.min(1.0, actionTextTimer / 400)) * 0.3;
        ctx.globalAlpha = alpha;
        ctx.fillStyle = "#4fc3f7";
        ctx.font = "bold " + Math.floor(22 * scale) + "px Consolas";
        var textW = actionTextStr.length * 11 * scale; // approximate
        ctx.fillText(actionTextStr,
                     BOARD_X + BOARD_W / 2 - textW / 2,
                     BOARD_Y + BOARD_H / 2 - 10);
        ctx.globalAlpha = 1.0;
    }

    ctx.restore();
}

// --- Main game loop ---
function gameLoop(timestamp) {
    requestAnimationFrame(gameLoop);

    var dt = timestamp - lastFrameTime;
    lastFrameTime = timestamp;

    // Cap delta time to avoid spiral of death
    if (dt > 100) dt = 100;
    if (dt < 0) dt = 0;

    update(dt);
    updateSequences();
    draw();
}

// --- Initialize ---
function init() {
    loadSettings();
    initAudio();
    calcLayout();
    setupMenuClicks();
    showMenu("menu-main");
    updateSettingsDisplay();
    lastFrameTime = performance.now();
    requestAnimationFrame(gameLoop);
}

init();
console.log("Tetris loaded!");

})();
