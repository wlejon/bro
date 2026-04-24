// audio.js — Gemswap-specific SFX layer on top of shared SFX module.
'use strict';
var G = G || {};

G.AppAudio = (function () {
    var settings = { sfxVol: 0.8, musicVol: 0.6 };

    function init(s) {
        if (s) settings = s;
        SFX.init({ sfxVol: settings.sfxVol, musicVol: settings.musicVol });
    }

    function pick() { SFX.tone(520, 0.06, 'sine', 0.35 * settings.sfxVol); }
    function cursor() { SFX.tone(380, 0.04, 'sine', 0.22 * settings.sfxVol); }

    function swap(valid) {
        if (valid) {
            SFX.sequence([[520, 0.05, 'sine', 0.4 * settings.sfxVol],
                          [720, 0.08, 'sine', 0.4 * settings.sfxVol]]);
        } else {
            SFX.tone(220, 0.12, 'square', 0.28 * settings.sfxVol);
        }
    }

    function match(chain, size) {
        // pitch climbs with chain, louder with size.
        var base = 440;
        var step = Math.min(chain, 8);
        var freq = base * Math.pow(1.122, step); // semitone per chain
        var vol = Math.min(1.0, 0.35 + size * 0.06);
        SFX.tone(freq, 0.14, 'triangle', vol * settings.sfxVol);
        if (size >= 4) {
            SFX.sequence([[freq * 1.5, 0.09, 'triangle', vol * 0.7 * settings.sfxVol],
                          [freq * 2.0, 0.12, 'sine', vol * 0.6 * settings.sfxVol]]);
        }
        if (chain >= 3) {
            // combo stinger — original synthesized motif (no voice samples)
            SFX.sequence([[600 + chain * 40, 0.06, 'square', 0.35 * settings.sfxVol],
                          [760 + chain * 40, 0.06, 'square', 0.35 * settings.sfxVol],
                          [960 + chain * 40, 0.10, 'square', 0.35 * settings.sfxVol]]);
        }
    }

    function hyper() {
        SFX.sequence([[300, 0.06, 'sawtooth', 0.5 * settings.sfxVol],
                      [500, 0.08, 'sawtooth', 0.5 * settings.sfxVol],
                      [800, 0.10, 'sawtooth', 0.5 * settings.sfxVol],
                      [1200, 0.14, 'triangle', 0.45 * settings.sfxVol]]);
    }

    function levelUp() {
        SFX.sequence([[523, 0.1, 'triangle', 0.5 * settings.sfxVol],
                      [659, 0.1, 'triangle', 0.5 * settings.sfxVol],
                      [784, 0.1, 'triangle', 0.5 * settings.sfxVol],
                      [1047, 0.18, 'sine', 0.5 * settings.sfxVol]]);
    }

    function shuffle() { SFX.tone(150, 0.25, 'sawtooth', 0.3 * settings.sfxVol); }
    function gameOver() { SFX.sequence([[440, 0.15, 'triangle', 0.45 * settings.sfxVol],
                                         [330, 0.15, 'triangle', 0.45 * settings.sfxVol],
                                         [220, 0.25, 'triangle', 0.45 * settings.sfxVol]]); }

    function menuMove() { SFX.tone(420, 0.04, 'sine', 0.3 * settings.sfxVol); }
    function menuSelect() { SFX.tone(660, 0.08, 'triangle', 0.45 * settings.sfxVol); }

    function applyVolume() {
        SFX.setSfxVol(settings.sfxVol);
        SFX.setMusicVol(settings.musicVol);
    }

    function setSettings(s) { settings = s; applyVolume(); }

    return {
        init: init, applyVolume: applyVolume, setSettings: setSettings,
        pick: pick, cursor: cursor, swap: swap, match: match, hyper: hyper,
        levelUp: levelUp, shuffle: shuffle, gameOver: gameOver,
        menuMove: menuMove, menuSelect: menuSelect,
    };
})();
