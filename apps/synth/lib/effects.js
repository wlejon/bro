// ---------------------------------------------------------------------------
// Effects — delay, reverb, chorus, compressor
// ---------------------------------------------------------------------------

(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var state = {
        delayEnabled: false, delayTime: 0.3, delayFeedback: 0.3, delayMix: 0.3,
        reverbEnabled: false, reverbRoomSize: 0.5, reverbDamping: 0.5, reverbMix: 0.2,
        chorusEnabled: false, chorusRate: 1.0, chorusDepth: 0.003, chorusMix: 0.3,
        chorusFeedback: 0, chorusBaseDelay: 0.007,
        compressorEnabled: false, compressorThreshold: -12, compressorRatio: 4,
        compressorAttack: 10, compressorRelease: 100
    };

    function ctx() { return Synth.getAudioContext(); }

    Synth.Effects = {
        init: function() {},

        // --- Delay ---
        setDelayEnabled: function(v) { state.delayEnabled = v; if (ctx()) ctx().setDelayEnabled(v); },
        isDelayEnabled: function() { return state.delayEnabled; },
        setDelayTime: function(v) { state.delayTime = v; if (ctx()) ctx().setDelayTime(v); },
        getDelayTime: function() { return state.delayTime; },
        setDelayFeedback: function(v) { state.delayFeedback = v; if (ctx()) ctx().setDelayFeedback(v); },
        getDelayFeedback: function() { return state.delayFeedback; },
        setDelayMix: function(v) { state.delayMix = v; if (ctx()) ctx().setDelayMix(v); },
        getDelayMix: function() { return state.delayMix; },

        // --- Reverb ---
        setReverbEnabled: function(v) { state.reverbEnabled = v; if (ctx()) ctx().setReverbEnabled(v); },
        isReverbEnabled: function() { return state.reverbEnabled; },
        setReverbRoomSize: function(v) { state.reverbRoomSize = v; if (ctx()) ctx().setReverbRoomSize(v); },
        getReverbRoomSize: function() { return state.reverbRoomSize; },
        setReverbDamping: function(v) { state.reverbDamping = v; if (ctx()) ctx().setReverbDamping(v); },
        getReverbDamping: function() { return state.reverbDamping; },
        setReverbMix: function(v) { state.reverbMix = v; if (ctx()) ctx().setReverbMix(v); },
        getReverbMix: function() { return state.reverbMix; },

        // --- Chorus ---
        setChorusEnabled: function(v) { state.chorusEnabled = v; if (ctx()) ctx().setChorusEnabled(v); },
        isChorusEnabled: function() { return state.chorusEnabled; },
        setChorusRate: function(v) { state.chorusRate = v; if (ctx()) ctx().setChorusRate(v); },
        getChorusRate: function() { return state.chorusRate; },
        setChorusDepth: function(v) { state.chorusDepth = v; if (ctx()) ctx().setChorusDepth(v); },
        getChorusDepth: function() { return state.chorusDepth; },
        setChorusMix: function(v) { state.chorusMix = v; if (ctx()) ctx().setChorusMix(v); },
        getChorusMix: function() { return state.chorusMix; },
        setChorusFeedback: function(v) { state.chorusFeedback = v; if (ctx()) ctx().setChorusFeedback(v); },
        getChorusFeedback: function() { return state.chorusFeedback; },
        setChorusBaseDelay: function(v) { state.chorusBaseDelay = v; if (ctx()) ctx().setChorusBaseDelay(v); },
        getChorusBaseDelay: function() { return state.chorusBaseDelay; },

        // --- Compressor ---
        setCompressorEnabled: function(v) { state.compressorEnabled = v; if (ctx()) ctx().setCompressorEnabled(v); },
        isCompressorEnabled: function() { return state.compressorEnabled; },
        setCompressorThreshold: function(v) { state.compressorThreshold = v; if (ctx()) ctx().setCompressorThreshold(v); },
        getCompressorThreshold: function() { return state.compressorThreshold; },
        setCompressorRatio: function(v) { state.compressorRatio = v; if (ctx()) ctx().setCompressorRatio(v); },
        getCompressorRatio: function() { return state.compressorRatio; },
        setCompressorAttack: function(v) { state.compressorAttack = v; if (ctx()) ctx().setCompressorAttack(v); },
        getCompressorAttack: function() { return state.compressorAttack; },
        setCompressorRelease: function(v) { state.compressorRelease = v; if (ctx()) ctx().setCompressorRelease(v); },
        getCompressorRelease: function() { return state.compressorRelease; },

        getState: function() {
            return {
                delayEnabled: state.delayEnabled, delayTime: state.delayTime,
                delayFeedback: state.delayFeedback, delayMix: state.delayMix,
                reverbEnabled: state.reverbEnabled, reverbRoomSize: state.reverbRoomSize,
                reverbDamping: state.reverbDamping, reverbMix: state.reverbMix,
                chorusEnabled: state.chorusEnabled, chorusRate: state.chorusRate,
                chorusDepth: state.chorusDepth, chorusMix: state.chorusMix,
                chorusFeedback: state.chorusFeedback, chorusBaseDelay: state.chorusBaseDelay,
                compressorEnabled: state.compressorEnabled,
                compressorThreshold: state.compressorThreshold,
                compressorRatio: state.compressorRatio,
                compressorAttack: state.compressorAttack,
                compressorRelease: state.compressorRelease
            };
        },

        loadState: function(s) {
            if (!s) return;
            Synth.Effects.setDelayTime(s.delayTime || 0.3);
            Synth.Effects.setDelayFeedback(s.delayFeedback || 0.3);
            Synth.Effects.setDelayMix(s.delayMix || 0.3);
            Synth.Effects.setDelayEnabled(s.delayEnabled || false);

            Synth.Effects.setReverbRoomSize(s.reverbRoomSize !== undefined ? s.reverbRoomSize : 0.5);
            Synth.Effects.setReverbDamping(s.reverbDamping !== undefined ? s.reverbDamping : 0.5);
            Synth.Effects.setReverbMix(s.reverbMix !== undefined ? s.reverbMix : 0.2);
            Synth.Effects.setReverbEnabled(s.reverbEnabled || false);

            Synth.Effects.setChorusRate(s.chorusRate !== undefined ? s.chorusRate : 1.0);
            Synth.Effects.setChorusDepth(s.chorusDepth !== undefined ? s.chorusDepth : 0.003);
            Synth.Effects.setChorusMix(s.chorusMix !== undefined ? s.chorusMix : 0.3);
            Synth.Effects.setChorusFeedback(s.chorusFeedback || 0);
            Synth.Effects.setChorusBaseDelay(s.chorusBaseDelay !== undefined ? s.chorusBaseDelay : 0.007);
            Synth.Effects.setChorusEnabled(s.chorusEnabled || false);

            Synth.Effects.setCompressorThreshold(s.compressorThreshold !== undefined ? s.compressorThreshold : -12);
            Synth.Effects.setCompressorRatio(s.compressorRatio !== undefined ? s.compressorRatio : 4);
            Synth.Effects.setCompressorAttack(s.compressorAttack !== undefined ? s.compressorAttack : 10);
            Synth.Effects.setCompressorRelease(s.compressorRelease !== undefined ? s.compressorRelease : 100);
            Synth.Effects.setCompressorEnabled(s.compressorEnabled || false);
        }
    };
})();
