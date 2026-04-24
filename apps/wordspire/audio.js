// audio.js — wordspire SFX. Ascending pitch per chain length, stingers.
'use strict';
var W = window.W = window.W || {};

W.Audio = (function () {
    function init() {
        return SFX.init({
            sfxVol: W.Storage.settings.sfxVol / 100,
            musicVol: W.Storage.settings.musicVol / 100
        });
    }

    // C major: C D E F G A B C' D' E'
    var LADDER = [261.63, 293.66, 329.63, 349.23, 392.00, 440.00, 493.88, 523.25, 587.33, 659.25, 783.99, 880.00];

    function tileAdd(n) {
        var i = Math.min(LADDER.length - 1, Math.max(0, n));
        SFX.tone(LADDER[i], 0.06, 'triangle', 0.45);
    }
    function tileRemove() { SFX.tone(180, 0.04, 'triangle', 0.3); }
    function clearChain() { SFX.tone(130, 0.08, 'sine', 0.3); }

    function submitOk(length) {
        // Bright chord ascending with length.
        var base = Math.min(7, Math.max(0, length - 3));
        SFX.sequence([
            [LADDER[base],     0.06, 'square', 0.55],
            [LADDER[base + 2], 0.06, 'square', 0.55],
            [LADDER[base + 4] || LADDER[LADDER.length - 1], 0.10, 'square', 0.65]
        ]);
    }
    function submitFail() {
        SFX.sequence([
            [220, 0.07, 'sawtooth', 0.5],
            [160, 0.12, 'sawtooth', 0.5]
        ]);
    }

    function fanfare() {
        SFX.sequence([
            [523.25, 0.08, 'square', 0.7],
            [659.25, 0.08, 'square', 0.7],
            [783.99, 0.08, 'square', 0.8],
            [1046.5, 0.18, 'square', 0.95]
        ]);
    }

    function sizzle() { SFX.tone(90, 0.18, 'sawtooth', 0.55); }
    function burnDrop() {
        SFX.sequence([
            [120, 0.05, 'sawtooth', 0.5],
            [90,  0.08, 'sawtooth', 0.5]
        ]);
    }

    function gameover() {
        SFX.sequence([
            [440, 0.18, 'sawtooth', 0.6],
            [330, 0.18, 'sawtooth', 0.55],
            [220, 0.30, 'sawtooth', 0.6],
            [165, 0.40, 'sawtooth', 0.55]
        ]);
    }
    function win() {
        SFX.sequence([
            [523, 0.08, 'square', 0.7],
            [659, 0.08, 'square', 0.7],
            [784, 0.08, 'square', 0.7],
            [1047, 0.22, 'square', 0.95]
        ]);
    }

    function menuMove()   { SFX.tone(420, 0.03, 'sine',   0.3); }
    function menuSelect() { SFX.tone(620, 0.08, 'square', 0.5); }

    return {
        init: init,
        tileAdd: tileAdd,
        tileRemove: tileRemove,
        clearChain: clearChain,
        submitOk: submitOk,
        submitFail: submitFail,
        fanfare: fanfare,
        sizzle: sizzle,
        burnDrop: burnDrop,
        gameover: gameover,
        win: win,
        menuMove: menuMove,
        menuSelect: menuSelect,
        setSfxVol: function (v) { SFX.setSfxVol(v); },
        setMusicVol: function (v) { SFX.setMusicVol(v); }
    };
})();
