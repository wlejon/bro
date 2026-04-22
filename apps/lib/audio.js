// audio.js — one-shot tones + volume buses over bro's AudioContext.
//
// A small layer over the pattern in every existing arcade app: short
// square/triangle/sine blips for menu / action feedback, mixed through a
// sfx bus and optional music bus. Degrades silently if AudioContext is
// unavailable (headless raster, pre-init, or user mute).
//
// Usage:
//   SFX.init();
//   SFX.tone(440, 0.08, "square", 0.6);
//   SFX.setSfxVol(0.8);

(function (global) {
    'use strict';

    const state = {
        ctx: null,
        sfxBus: -1,
        musicBus: -1,
        sfxVol: 1.0,
        musicVol: 1.0,
        inited: false,
    };

    function init(opts) {
        if (state.inited) return !!state.ctx;
        state.inited = true;
        opts = opts || {};
        state.sfxVol   = opts.sfxVol   != null ? opts.sfxVol   : 1.0;
        state.musicVol = opts.musicVol != null ? opts.musicVol : 1.0;
        try { state.ctx = new AudioContext(); }
        catch (e) { state.ctx = null; return false; }
        try {
            state.sfxBus   = state.ctx.createBus();
            state.musicBus = state.ctx.createBus();
            state.ctx.setBusGain(state.sfxBus,   state.sfxVol);
            state.ctx.setBusGain(state.musicBus, state.musicVol);
            state.ctx.setCompressorEnabled(true);
            state.ctx.setCompressorThreshold(-12);
            state.ctx.setCompressorRatio(3);
        } catch (e) {
            state.sfxBus   = -1;
            state.musicBus = -1;
        }
        return true;
    }

    // Short ADSR blip. `type` = "square" | "sine" | "triangle" | "sawtooth" | "whitenoise".
    function tone(freq, duration, type, vol) {
        const ctx = state.ctx;
        if (!ctx) return;
        const v = (vol != null ? vol : 1.0);
        if (v <= 0) return;
        try {
            const id = ctx.createVoice();
            ctx.setVoiceWaveform(id, type || 'square');
            if (freq > 0) ctx.setVoiceFrequency(id, freq);
            ctx.setVoiceGain(id, v * 15.0);
            ctx.setVoiceAttack(id, 0.003);
            ctx.setVoiceDecay(id, Math.max(0.01, duration * 0.8));
            ctx.setVoiceSustain(id, 0.0);
            ctx.setVoiceRelease(id, 0.02);
            if (state.sfxBus !== -1) ctx.setVoiceBus(id, state.sfxBus);
            const t = ctx.currentTime;
            ctx.startVoice(id, t);
            ctx.stopVoice(id, t + duration);
        } catch (e) {}
    }

    // Play a short arpeggio: [ [freq, dur, type?, vol?], ... ], each entry
    // fired sequentially via setTimeout. Handy for chimes / stingers.
    function sequence(notes) {
        let acc = 0;
        for (const n of notes) {
            const [f, d, t, v] = n;
            setTimeout(() => tone(f, d, t, v), acc * 1000);
            acc += d;
        }
    }

    function setSfxVol(v) {
        state.sfxVol = Math.max(0, Math.min(1, v));
        if (state.ctx && state.sfxBus !== -1) {
            try { state.ctx.setBusGain(state.sfxBus, state.sfxVol); } catch (e) {}
        }
    }

    function setMusicVol(v) {
        state.musicVol = Math.max(0, Math.min(1, v));
        if (state.ctx && state.musicBus !== -1) {
            try { state.ctx.setBusGain(state.musicBus, state.musicVol); } catch (e) {}
        }
    }

    global.SFX = {
        init, tone, sequence, setSfxVol, setMusicVol,
        ctx:       () => state.ctx,
        sfxBus:    () => state.sfxBus,
        musicBus:  () => state.musicBus,
    };
})(typeof window !== 'undefined' ? window : globalThis);
