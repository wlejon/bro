// audio.js — SFX layer over lib/audio (SFX).
var A = A || {};

function _noise(dur, vol, freq) {
    var ctx = SFX.ctx();
    if (!ctx) return;
    try {
        var id = ctx.createVoice();
        ctx.setVoiceWaveform(id, "noise");
        ctx.setVoiceFrequency(id, freq || 200);
        ctx.setVoiceGain(id, (vol || 1.0) * 12.0);
        ctx.setVoiceAttack(id, 0.003);
        ctx.setVoiceDecay(id, dur * 0.7);
        ctx.setVoiceSustain(id, 0);
        ctx.setVoiceRelease(id, 0.05);
        var bus = SFX.sfxBus();
        if (bus !== -1) ctx.setVoiceBus(id, bus);
        var t = ctx.currentTime;
        ctx.startVoice(id, t);
        ctx.stopVoice(id, t + dur);
    } catch (e) {}
}

A.Audio = {
    init: function() { SFX.init(); },
    sfxFire:        function() { SFX.tone(880, 0.08, "square", 0.3); },
    sfxBangLarge:   function() { _noise(0.4,  0.9, 120); },
    sfxBangMed:     function() { _noise(0.25, 0.8, 180); },
    sfxBangSmall:   function() { _noise(0.15, 0.7, 260); },
    sfxShipExplode: function() {
        _noise(0.7, 1.0, 80);
        setTimeout(function() { _noise(0.3, 0.6, 150); }, 120);
    },
    sfxMenuMove:   function() { SFX.tone(400, 0.03, "sine",   0.3); },
    sfxMenuSelect: function() { SFX.tone(600, 0.08, "square", 0.4); },
    sfxExtraLife:  function() { SFX.sequence([[523,0.08,"square",0.6],[659,0.08,"square",0.6],[784,0.12,"square",0.7]]); },
    sfxWave:       function() { SFX.tone(330, 0.15, "triangle", 0.6); },
};
