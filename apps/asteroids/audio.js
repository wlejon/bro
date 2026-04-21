// audio.js — Simple SFX via broaudio
var A = A || {};

A.Audio = {
    ctx: null,
    sfxBus: -1,
    thrustVoice: -1,
    thrustOn: false,

    init: function() {
        try { this.ctx = new AudioContext(); } catch(e) { this.ctx = null; return; }
        try {
            this.sfxBus = this.ctx.createBus();
            this.ctx.setBusGain(this.sfxBus, 0.8);
        } catch(e) { this.sfxBus = -1; }
    },

    playTone: function(freq, duration, type, vol) {
        if (!this.ctx) return;
        var v = (vol !== undefined ? vol : 1.0) * 0.8;
        if (v <= 0) return;
        try {
            var id = this.ctx.createVoice();
            this.ctx.setVoiceWaveform(id, type || "square");
            this.ctx.setVoiceFrequency(id, freq);
            this.ctx.setVoiceGain(id, v * 15.0);
            this.ctx.setVoiceAttack(id, 0.003);
            this.ctx.setVoiceDecay(id, duration * 0.8);
            this.ctx.setVoiceSustain(id, 0.0);
            this.ctx.setVoiceRelease(id, 0.02);
            if (this.sfxBus !== -1) this.ctx.setVoiceBus(id, this.sfxBus);
            var t = this.ctx.currentTime;
            this.ctx.startVoice(id, t);
            this.ctx.stopVoice(id, t + duration);
        } catch(e) {}
    },

    playNoise: function(duration, vol, freq) {
        if (!this.ctx) return;
        try {
            var id = this.ctx.createVoice();
            this.ctx.setVoiceWaveform(id, "noise");
            this.ctx.setVoiceFrequency(id, freq || 200);
            this.ctx.setVoiceGain(id, (vol || 1.0) * 12.0);
            this.ctx.setVoiceAttack(id, 0.003);
            this.ctx.setVoiceDecay(id, duration * 0.7);
            this.ctx.setVoiceSustain(id, 0.0);
            this.ctx.setVoiceRelease(id, 0.05);
            if (this.sfxBus !== -1) this.ctx.setVoiceBus(id, this.sfxBus);
            var t = this.ctx.currentTime;
            this.ctx.startVoice(id, t);
            this.ctx.stopVoice(id, t + duration);
        } catch(e) {}
    },

    sfxFire: function()     { this.playTone(880, 0.08, "square", 0.3); },
    sfxBangLarge: function(){ this.playNoise(0.4, 0.9, 120); },
    sfxBangMed: function()  { this.playNoise(0.25, 0.8, 180); },
    sfxBangSmall: function(){ this.playNoise(0.15, 0.7, 260); },
    sfxShipExplode: function() {
        this.playNoise(0.7, 1.0, 80);
        var self = this;
        setTimeout(function() { self.playNoise(0.3, 0.6, 150); }, 120);
    },
    sfxMenuMove: function()   { this.playTone(400, 0.03, "sine", 0.3); },
    sfxMenuSelect: function() { this.playTone(600, 0.08, "square", 0.4); },
    sfxExtraLife: function() {
        var self = this;
        this.playTone(523, 0.08, "square", 0.6);
        setTimeout(function() { self.playTone(659, 0.08, "square", 0.6); }, 80);
        setTimeout(function() { self.playTone(784, 0.12, "square", 0.7); }, 160);
    },
    sfxWave: function()       { this.playTone(330, 0.15, "triangle", 0.6); }
};
