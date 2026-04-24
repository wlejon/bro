// audio.js — Fluffshuffle SFX: per-color pitches, cascade steps, drag whoosh,
// snap click, pop squeak, lock thud.
'use strict';
var G = G || {};

G.AppAudio = (function () {
    var settings = { sfxVol: 0.8, musicVol: 0.6 };

    // Distinct base pitch per color so matches also carry aural identity.
    var COLOR_PITCH = [0, 440, 494, 523, 587, 659, 784];

    function init(s) {
        if (s) settings = s;
        SFX.init({ sfxVol: settings.sfxVol, musicVol: settings.musicVol });
    }

    function setSettings(s) {
        settings = s;
        SFX.setSfxVol(settings.sfxVol);
        SFX.setMusicVol(settings.musicVol);
    }

    function grab()    { SFX.tone(520, 0.05, 'sine',     0.35 * settings.sfxVol); }
    function snap()    { SFX.tone(880, 0.04, 'square',   0.30 * settings.sfxVol); }
    function drag()    { SFX.tone(180, 0.10, 'sawtooth', 0.20 * settings.sfxVol); }
    function cursor()  { SFX.tone(380, 0.04, 'sine',     0.22 * settings.sfxVol); }
    function thud()    { SFX.tone(120, 0.16, 'sawtooth', 0.40 * settings.sfxVol); }
    function lock()    { SFX.sequence([[260, 0.06, 'triangle', 0.35 * settings.sfxVol],
                                       [180, 0.10, 'triangle', 0.35 * settings.sfxVol]]); }

    function match(chain, color, size) {
        var base = COLOR_PITCH[color] || 520;
        var step = Math.min(chain, 8);
        var freq = base * Math.pow(1.0595, step); // semitone climb per cascade
        var vol = Math.min(1.0, 0.35 + size * 0.05);
        SFX.tone(freq, 0.12, 'triangle', vol * settings.sfxVol);
        // Little squeak for the pop itself.
        SFX.tone(freq * 1.5, 0.06, 'sine', 0.25 * vol * settings.sfxVol);
        if (size >= 4) {
            SFX.sequence([[freq * 1.5, 0.08, 'triangle', vol * 0.7 * settings.sfxVol],
                          [freq * 2.0, 0.12, 'sine',     vol * 0.55 * settings.sfxVol]]);
        }
        if (chain >= 3) {
            SFX.sequence([[600 + chain * 50, 0.06, 'square', 0.30 * settings.sfxVol],
                          [760 + chain * 50, 0.06, 'square', 0.30 * settings.sfxVol],
                          [960 + chain * 50, 0.10, 'square', 0.30 * settings.sfxVol]]);
        }
    }

    function levelUp() {
        SFX.sequence([[523, 0.1, 'triangle', 0.5 * settings.sfxVol],
                      [659, 0.1, 'triangle', 0.5 * settings.sfxVol],
                      [784, 0.1, 'triangle', 0.5 * settings.sfxVol],
                      [1047, 0.18, 'sine',   0.5 * settings.sfxVol]]);
    }

    function gameOver() {
        SFX.sequence([[440, 0.15, 'triangle', 0.45 * settings.sfxVol],
                      [330, 0.15, 'triangle', 0.45 * settings.sfxVol],
                      [220, 0.25, 'triangle', 0.45 * settings.sfxVol]]);
    }

    function menuMove()   { SFX.tone(420, 0.04, 'sine',     0.30 * settings.sfxVol); }
    function menuSelect() { SFX.tone(660, 0.08, 'triangle', 0.45 * settings.sfxVol); }

    return {
        init: init, setSettings: setSettings,
        grab: grab, snap: snap, drag: drag, cursor: cursor, thud: thud, lock: lock,
        match: match, levelUp: levelUp, gameOver: gameOver,
        menuMove: menuMove, menuSelect: menuSelect,
    };
})();
