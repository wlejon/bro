// audio.js — SFX layer over lib/audio (SFX). Original cues only — no samples.
var N = N || {};

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

// Pitched sweep — rising or falling glide used for lock tone and enemy sweep.
function _sweep(f0, f1, dur, wave, vol) {
    var ctx = SFX.ctx();
    if (!ctx) return;
    try {
        var id = ctx.createVoice();
        ctx.setVoiceWaveform(id, wave || "square");
        ctx.setVoiceFrequency(id, f0);
        ctx.setVoiceGain(id, (vol || 0.4) * 12.0);
        ctx.setVoiceAttack(id, 0.002);
        ctx.setVoiceDecay(id, dur);
        ctx.setVoiceSustain(id, 0);
        ctx.setVoiceRelease(id, 0.02);
        var bus = SFX.sfxBus();
        if (bus !== -1) ctx.setVoiceBus(id, bus);
        var t = ctx.currentTime;
        ctx.startVoice(id, t);
        // Linear frequency slide emulated via two stops (we lack automation hooks
        // on SFX; approximate with a second tone crossfaded in).
        ctx.stopVoice(id, t + dur);
        var id2 = ctx.createVoice();
        ctx.setVoiceWaveform(id2, wave || "square");
        ctx.setVoiceFrequency(id2, f1);
        ctx.setVoiceGain(id2, (vol || 0.4) * 12.0);
        ctx.setVoiceAttack(id2, dur * 0.5);
        ctx.setVoiceDecay(id2, dur * 0.5);
        ctx.setVoiceSustain(id2, 0);
        ctx.setVoiceRelease(id2, 0.02);
        if (bus !== -1) ctx.setVoiceBus(id2, bus);
        ctx.startVoice(id2, t);
        ctx.stopVoice(id2, t + dur);
    } catch (e) {}
}

N.Audio = {
    init: function() { SFX.init(); },

    // --- Weapons ---
    sfxLaser:       function() { _sweep(1100, 500, 0.14, "square", 0.35); },
    sfxEnemyLaser:  function() { _sweep(700, 350, 0.16, "square", 0.25); },

    // --- Hits ---
    sfxEnemyHit:    function() { _noise(0.22, 0.7, 260); },
    sfxEnemyBoom:   function() { _noise(0.45, 1.0, 140); },
    sfxTowerBoom:   function() { _noise(0.55, 1.0, 100); },
    sfxShieldHit:   function() {
        _noise(0.32, 1.0, 80);
        SFX.tone(180, 0.22, "sawtooth", 0.5);
    },
    sfxShipExplode: function() {
        _noise(0.9, 1.0, 70);
        setTimeout(function() { _noise(0.5, 0.8, 130); }, 140);
        setTimeout(function() { _noise(0.3, 0.6, 200); }, 320);
    },

    // --- Pacing / UI ---
    sfxFireball:    function() { _sweep(240, 90, 0.6, "sawtooth", 0.35); },
    sfxAceEngine:   function() { _sweep(520, 320, 0.8, "sawtooth", 0.32); },
    sfxLockTone:    function() { SFX.tone(1400, 0.05, "square", 0.35); },
    sfxBullseye:    function() { SFX.sequence([[523,0.10,"square",0.6],[659,0.10,"square",0.7],[784,0.10,"square",0.8],[1047,0.3,"square",0.9]]); },
    sfxDirectHit:   function() { SFX.sequence([[523,0.10,"square",0.6],[784,0.22,"square",0.8]]); },
    sfxWave:        function() { SFX.sequence([[330,0.10,"triangle",0.6],[440,0.10,"triangle",0.6],[554,0.18,"triangle",0.7]]); },
    sfxMenuMove:    function() { SFX.tone(400, 0.03, "sine",   0.3); },
    sfxMenuSelect:  function() { SFX.tone(600, 0.08, "square", 0.4); },
    sfxBonusShield: function() { SFX.sequence([[659,0.08,"triangle",0.5],[880,0.08,"triangle",0.6],[1175,0.16,"triangle",0.7]]); }
};
