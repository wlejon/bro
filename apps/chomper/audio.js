// audio.js — SFX helpers wrapping lib/audio (SFX).
var P = P || {};

P.Audio = {
    chompToggle: false,
    init: function() { SFX.init(); },
    sfxChomp: function() {
        this.chompToggle = !this.chompToggle;
        SFX.tone(this.chompToggle ? 440 : 330, 0.04, "square", 0.3);
    },
    sfxPower:    function() { SFX.tone(220, 0.3, "sawtooth", 0.5); },
    sfxEatGhost: function() { SFX.sequence([[523,0.08,"square",0.6],[659,0.08,"square",0.6],[784,0.12,"square",0.7]]); },
    sfxDeath:    function() { SFX.sequence([[400,0.15,"sawtooth",0.6],[300,0.15,"sawtooth",0.6],[200,0.3,"sawtooth",0.6]]); },
    sfxWin:      function() { SFX.sequence([[523,0.1,"square",0.7],[659,0.1,"square",0.7],[784,0.1,"square",0.7],[1047,0.2,"square",0.8]]); },
    sfxMenu:     function() { SFX.tone(500, 0.04, "sine", 0.3); },
};
