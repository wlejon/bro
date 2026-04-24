// audio.js — short sfx for Serpcoil layered on top of apps/lib/audio.js (SFX).
var SC = SC || {};

SC.Audio = (function () {
    "use strict";

    // Pentatonic color tones, loaded from SC.Chain.COLORS when available.
    function colorTone(color) {
        var c = SC.Chain && SC.Chain.COLORS && SC.Chain.COLORS[color];
        return c ? c.tone : 440;
    }

    function sfxPop(color, combo) {
        var f = colorTone(color);
        var mult = combo > 1 ? Math.pow(2, (combo - 1) / 12) : 1;
        SFX.tone(f * mult, 0.14, "square", 0.5);
    }

    function sfxInsert() { SFX.tone(380, 0.05, "square", 0.3); }
    function sfxShoot()  { SFX.tone(220, 0.06, "sawtooth", 0.4); }
    function sfxSwap()   { SFX.tone(540, 0.07, "sine", 0.35); }
    function sfxMarch()  { SFX.tone(90, 0.04, "triangle", 0.15); }
    function sfxDanger() { SFX.tone(180, 0.18, "sawtooth", 0.5); }
    function sfxClear()  {
        SFX.sequence([
            [523, 0.12, "square", 0.6],
            [659, 0.12, "square", 0.6],
            [784, 0.12, "square", 0.7],
            [1047, 0.25, "square", 0.8]
        ]);
    }
    function sfxGameOver() {
        SFX.sequence([
            [300, 0.18, "sawtooth", 0.5],
            [240, 0.18, "sawtooth", 0.5],
            [180, 0.36, "sawtooth", 0.55]
        ]);
    }
    function sfxPowerup() { SFX.tone(880, 0.2, "square", 0.7); }
    function sfxMenu()    { SFX.tone(420, 0.035, "sine", 0.3); }
    function sfxSelect()  { SFX.tone(640, 0.08, "square", 0.4); }

    return {
        sfxPop: sfxPop,
        sfxInsert: sfxInsert,
        sfxShoot: sfxShoot,
        sfxSwap: sfxSwap,
        sfxMarch: sfxMarch,
        sfxDanger: sfxDanger,
        sfxClear: sfxClear,
        sfxGameOver: sfxGameOver,
        sfxPowerup: sfxPowerup,
        sfxMenu: sfxMenu,
        sfxSelect: sfxSelect
    };
})();
