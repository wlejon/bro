// audio.js — fintank SFX.
'use strict';
var F = window.F = window.F || {};

F.Audio = (function () {
    function init() {
        return SFX.init({
            sfxVol: F.Economy.settings.sfxVol / 100,
            musicVol: F.Economy.settings.musicVol / 100
        });
    }

    function feed()        { SFX.tone(280, 0.06, 'triangle', 0.4); }
    function splash()      { SFX.tone(180, 0.05, 'sine',     0.3); }
    function chomp()       { SFX.tone(220, 0.04, 'square',   0.35); }
    function coinDrop(tier){ var f = 440 + (tier||1) * 80; SFX.tone(f, 0.05, 'sine', 0.45); }
    function coinGet(tier) { var f = 620 + (tier||1) * 120; SFX.tone(f, 0.06, 'triangle', 0.5); }
    function hit()         { SFX.tone(140, 0.05, 'sawtooth', 0.5); }
    function intruderRoar(){
        SFX.sequence([
            [90, 0.08, 'sawtooth', 0.55],
            [70, 0.10, 'sawtooth', 0.55]
        ]);
    }
    function intruderDie() {
        SFX.sequence([
            [220, 0.05, 'square', 0.55],
            [160, 0.05, 'square', 0.55],
            [100, 0.10, 'square', 0.45]
        ]);
    }
    function fishDie()     { SFX.tone(120, 0.20, 'sawtooth', 0.45); }
    function pet()         { SFX.tone(640, 0.05, 'triangle', 0.4); }
    function hatch() {
        SFX.sequence([
            [500, 0.05, 'square', 0.5],
            [620, 0.05, 'square', 0.5],
            [780, 0.08, 'square', 0.6]
        ]);
    }
    function dayEnd() {
        SFX.sequence([
            [523, 0.08, 'square', 0.6],
            [659, 0.08, 'square', 0.6],
            [784, 0.08, 'square', 0.6],
            [1047, 0.18, 'square', 0.85]
        ]);
    }
    function gameover() {
        SFX.sequence([
            [440, 0.18, 'sawtooth', 0.5],
            [330, 0.18, 'sawtooth', 0.5],
            [220, 0.30, 'sawtooth', 0.5]
        ]);
    }
    function buy()        { SFX.tone(520, 0.05, 'square', 0.45); }
    function buyFail()    { SFX.tone(180, 0.08, 'sawtooth', 0.45); }
    function menuMove()   { SFX.tone(420, 0.03, 'sine',   0.3); }
    function menuSelect() { SFX.tone(620, 0.07, 'square', 0.45); }

    return {
        init: init,
        feed: feed, splash: splash, chomp: chomp,
        coinDrop: coinDrop, coinGet: coinGet,
        hit: hit,
        intruderRoar: intruderRoar, intruderDie: intruderDie,
        fishDie: fishDie,
        pet: pet, hatch: hatch,
        dayEnd: dayEnd, gameover: gameover,
        buy: buy, buyFail: buyFail,
        menuMove: menuMove, menuSelect: menuSelect,
        setSfxVol:   function (v) { SFX.setSfxVol(v); },
        setMusicVol: function (v) { SFX.setMusicVol(v); }
    };
})();
