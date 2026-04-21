// audio.js — simple SFX via broaudio (optional)
var P = P || {};

P.Audio = {
    ctx: null,

    init: function() {
        try {
            if (typeof AudioContext !== "undefined") {
                this.ctx = new AudioContext();
            }
        } catch(e) { this.ctx = null; }
    },

    playTone: function(freq, duration, type, vol) {
        if (!this.ctx) return;
        try {
            var id = this.ctx.createVoice();
            this.ctx.setVoiceWaveform(id, type || "square");
            this.ctx.setVoiceFrequency(id, freq);
            this.ctx.setVoiceGain(id, (vol !== undefined ? vol : 0.5) * 12.0);
            this.ctx.setVoiceAttack(id, 0.003);
            this.ctx.setVoiceDecay(id, duration * 0.8);
            this.ctx.setVoiceSustain(id, 0.0);
            this.ctx.setVoiceRelease(id, 0.02);
            var t = this.ctx.currentTime;
            this.ctx.startVoice(id, t);
            this.ctx.stopVoice(id, t + duration);
        } catch(e) {}
    },

    chompToggle: false,
    sfxChomp: function() {
        this.chompToggle = !this.chompToggle;
        this.playTone(this.chompToggle ? 440 : 330, 0.04, "square", 0.3);
    },
    sfxPower: function() {
        this.playTone(220, 0.3, "sawtooth", 0.5);
    },
    sfxEatGhost: function() {
        var self = this;
        this.playTone(523, 0.08, "square", 0.6);
        setTimeout(function() { self.playTone(659, 0.08, "square", 0.6); }, 60);
        setTimeout(function() { self.playTone(784, 0.12, "square", 0.7); }, 120);
    },
    sfxDeath: function() {
        var self = this;
        this.playTone(400, 0.15, "sawtooth", 0.6);
        setTimeout(function() { self.playTone(300, 0.15, "sawtooth", 0.6); }, 150);
        setTimeout(function() { self.playTone(200, 0.3, "sawtooth", 0.6); }, 300);
    },
    sfxWin: function() {
        var self = this;
        this.playTone(523, 0.1, "square", 0.7);
        setTimeout(function() { self.playTone(659, 0.1, "square", 0.7); }, 100);
        setTimeout(function() { self.playTone(784, 0.1, "square", 0.7); }, 200);
        setTimeout(function() { self.playTone(1047, 0.2, "square", 0.8); }, 300);
    },
    sfxMenu: function() {
        this.playTone(500, 0.04, "sine", 0.3);
    }
};
