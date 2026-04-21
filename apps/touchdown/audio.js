// audio.js — Simple SFX via broaudio
var T = T || {};

T.Audio = {
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

    startThrust: function() {
        if (!this.ctx) return;
        if (this.thrustOn) return;
        try {
            var id = this.ctx.createVoice();
            this.ctx.setVoiceWaveform(id, "noise");
            this.ctx.setVoiceFrequency(id, 300);
            this.ctx.setVoiceGain(id, 4.0);
            this.ctx.setVoiceAttack(id, 0.02);
            this.ctx.setVoiceDecay(id, 0.1);
            this.ctx.setVoiceSustain(id, 1.0);
            this.ctx.setVoiceRelease(id, 0.08);
            if (this.sfxBus !== -1) this.ctx.setVoiceBus(id, this.sfxBus);
            this.thrustVoice = id;
            this.ctx.startVoice(id, this.ctx.currentTime);
            this.thrustOn = true;
        } catch(e) {}
    },

    stopThrust: function() {
        if (!this.ctx) return;
        if (!this.thrustOn) return;
        try {
            if (this.thrustVoice !== -1) {
                this.ctx.stopVoice(this.thrustVoice, this.ctx.currentTime);
            }
        } catch(e) {}
        this.thrustVoice = -1;
        this.thrustOn = false;
    },

    sfxCrash: function() {
        this.playNoise(0.7, 1.0, 70);
        var self = this;
        setTimeout(function() { self.playNoise(0.35, 0.7, 130); }, 110);
    },
    sfxLanded: function() {
        var self = this;
        this.playTone(523, 0.09, "square", 0.6);
        setTimeout(function() { self.playTone(659, 0.09, "square", 0.6); }, 90);
        setTimeout(function() { self.playTone(784, 0.14, "square", 0.7); }, 180);
    },
    sfxMenuMove: function()   { this.playTone(400, 0.03, "sine", 0.3); },
    sfxMenuSelect: function() { this.playTone(600, 0.08, "square", 0.4); }
};
