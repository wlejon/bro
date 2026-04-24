// audio.js — Blockpop SFX. Diatonic pitch ladder per color, pop chains.
'use strict';
var G = window.G = window.G || {};

G.Audio = (function () {
    function initAudio() {
        var ok = SFX.init({
            sfxVol: G.Storage.settings.sfxVol / 100,
            musicVol: G.Storage.settings.musicVol / 100
        });
        return ok;
    }

    // Diatonic C major scale starting at C4 (261.63): C D E F G A B c'
    var LADDER = [261.63, 293.66, 329.63, 349.23, 392.0, 440.0, 493.88, 523.25];

    function pick() { SFX.tone(660, 0.05, 'triangle', 0.35); }
    function drop() { SFX.tone(220, 0.08, 'triangle', 0.5); }
    function move() { SFX.tone(440, 0.02, 'square', 0.15); }
    function shuffle() { SFX.tone(320, 0.04, 'sine', 0.25); }
    function brake() { SFX.sequence([[180, 0.08, 'sawtooth', 0.5], [260, 0.08, 'sawtooth', 0.4]]); }

    function pop(color, chainDepth) {
        var c = ((color | 0) - 1) % LADDER.length;
        if (c < 0) c = 0;
        var mult = Math.pow(2, Math.min(2, chainDepth)); // up to 4x freq
        SFX.tone(LADDER[c] * mult, 0.08, 'square', 0.55);
    }

    function big(pops) {
        // Cascade fanfare
        var notes = [];
        for (var i = 0; i < Math.min(5, pops); i++) {
            notes.push([LADDER[i % LADDER.length] * 2, 0.06, 'square', 0.6]);
        }
        SFX.sequence(notes);
    }

    function warn() { SFX.tone(90, 0.22, 'sawtooth', 0.6); }
    function gameover() {
        SFX.sequence([
            [440, 0.18, 'sawtooth', 0.6],
            [330, 0.18, 'sawtooth', 0.55],
            [220, 0.32, 'sawtooth', 0.6]
        ]);
    }
    function levelUp() {
        SFX.sequence([
            [392, 0.08, 'square', 0.6],
            [523, 0.08, 'square', 0.7],
            [659, 0.14, 'square', 0.8]
        ]);
    }
    function menuMove() { SFX.tone(420, 0.03, 'sine', 0.3); }
    function menuSelect() { SFX.tone(620, 0.08, 'square', 0.5); }
    function win() {
        SFX.sequence([
            [523, 0.08, 'square', 0.7],
            [659, 0.08, 'square', 0.7],
            [784, 0.08, 'square', 0.7],
            [1047, 0.22, 'square', 0.9]
        ]);
    }
    function special(kind) {
        if (kind === 'star') {
            SFX.sequence([
                [784, 0.05, 'square', 0.6],
                [988, 0.05, 'square', 0.6],
                [1318, 0.1, 'square', 0.7]
            ]);
        } else if (kind === 'bomb') {
            SFX.tone(60, 0.28, 'sawtooth', 0.8);
        } else if (kind === 'rainbow') {
            SFX.sequence([
                [523, 0.04, 'sine', 0.5], [659, 0.04, 'sine', 0.5],
                [784, 0.04, 'sine', 0.5], [988, 0.08, 'sine', 0.6]
            ]);
        }
    }

    function setSfxVol(v) { SFX.setSfxVol(v); }

    return {
        init: initAudio,
        pick: pick, drop: drop, move: move, shuffle: shuffle, brake: brake,
        pop: pop, big: big, warn: warn, gameover: gameover,
        levelUp: levelUp, win: win, special: special,
        menuMove: menuMove, menuSelect: menuSelect,
        setSfxVol: setSfxVol
    };
})();
