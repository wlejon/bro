// audio.js — SFX kit for Pegbounce. All original, all synth.
// Peg-hit pitch climbs a chromatic ladder with combo count.

'use strict';
(function (global) {

    // Chromatic ladder of frequencies: C4 to C6 and then saturates.
    const LADDER = (function () {
        const out = [];
        const base = 261.63; // C4
        for (let i = 0; i < 36; i++) out.push(base * Math.pow(2, i / 12));
        return out;
    })();

    function ladderFreq(combo) {
        const i = Math.min(LADDER.length - 1, Math.max(0, combo | 0));
        return LADDER[i];
    }

    const Sfx = {
        init(vol) { SFX.init({ sfxVol: vol || 0.8 }); },
        pegHit(combo) {
            const f = ladderFreq(combo);
            SFX.tone(f, 0.06, 'triangle', 0.45);
        },
        orangeHit(combo) {
            const f = ladderFreq(combo) * 1.5;
            SFX.tone(f, 0.12, 'square', 0.5);
        },
        greenHit() {
            SFX.sequence([
                [660, 0.06, 'triangle', 0.55],
                [880, 0.08, 'triangle', 0.55],
                [1320, 0.10, 'square', 0.6],
            ]);
        },
        purpleHit() {
            SFX.sequence([
                [520, 0.06, 'sawtooth', 0.45],
                [780, 0.08, 'sawtooth', 0.5],
            ]);
        },
        wallHit()   { SFX.tone(140, 0.03, 'square', 0.2); },
        launch()    { SFX.tone(480, 0.09, 'triangle', 0.45); },
        catchGet()  { SFX.sequence([[700,0.06,'square',0.5],[950,0.06,'square',0.55],[1250,0.1,'triangle',0.6]]); },
        levelClear(){ SFX.sequence([
                         [523,0.1,'square',0.55], [659,0.1,'square',0.6],
                         [784,0.1,'square',0.65], [1047,0.2,'triangle',0.7]
                      ]); },
        levelFail() { SFX.sequence([[250,0.15,'sawtooth',0.5],[180,0.2,'sawtooth',0.55]]); },
        fever()     { SFX.sequence([[330,0.08,'sawtooth',0.5],[440,0.08,'sawtooth',0.55],[660,0.08,'square',0.6],[880,0.18,'square',0.7]]); },
        menuMove()  { SFX.tone(420, 0.03, 'sine', 0.35); },
        menuSelect(){ SFX.tone(660, 0.06, 'square', 0.45); },
    };

    global.Sfx = Sfx;
})(typeof window !== 'undefined' ? window : globalThis);
