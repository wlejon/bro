// audio.js — SFX layer over lib/audio (SFX). Keeps a persistent thrust
// voice across thrust toggles since SFX.tone is one-shot.
var T = T || {};

T.Audio = {
    thrustVoice: -1,
    thrustOn: false,

    init: function() { SFX.init(); },

    startThrust: function() {
        var ctx = SFX.ctx();
        if (!ctx || this.thrustOn) return;
        try {
            var id = ctx.createVoice();
            ctx.setVoiceWaveform(id, "noise");
            ctx.setVoiceFrequency(id, 300);
            ctx.setVoiceGain(id, 4.0);
            ctx.setVoiceAttack(id, 0.02);
            ctx.setVoiceDecay(id, 0.1);
            ctx.setVoiceSustain(id, 1.0);
            ctx.setVoiceRelease(id, 0.08);
            var bus = SFX.sfxBus();
            if (bus !== -1) ctx.setVoiceBus(id, bus);
            ctx.startVoice(id, ctx.currentTime);
            this.thrustVoice = id;
            this.thrustOn = true;
        } catch (e) {}
    },
    stopThrust: function() {
        var ctx = SFX.ctx();
        if (!ctx || !this.thrustOn) return;
        try { if (this.thrustVoice !== -1) ctx.stopVoice(this.thrustVoice, ctx.currentTime); } catch (e) {}
        this.thrustVoice = -1;
        this.thrustOn = false;
    },

    sfxCrash: function() {
        var ctx = SFX.ctx();
        if (!ctx) return;
        // Two-stage noise burst via SFX.tone won't work (tone is oscillator);
        // emit noise directly since SFX doesn't expose a noise helper.
        function noise(dur, vol, freq) {
            try {
                var id = ctx.createVoice();
                ctx.setVoiceWaveform(id, "noise");
                ctx.setVoiceFrequency(id, freq);
                ctx.setVoiceGain(id, vol * 12);
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
        noise(0.7, 1.0, 70);
        setTimeout(function() { noise(0.35, 0.7, 130); }, 110);
    },
    sfxLanded: function() {
        SFX.sequence([
            [523, 0.09, "square", 0.6],
            [659, 0.09, "square", 0.6],
            [784, 0.14, "square", 0.7],
        ]);
    },
    sfxMenuMove:   function() { SFX.tone(400, 0.03, "sine",   0.3); },
    sfxMenuSelect: function() { SFX.tone(600, 0.08, "square", 0.4); },
};
