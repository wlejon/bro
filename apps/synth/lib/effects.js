(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var delayEnabled = false;
    var delayTime = 0.3;
    var delayFeedback = 0.3;
    var delayMix = 0.3;

    Synth.Effects = {
        init: function() {},

        setDelayEnabled: function(enabled) {
            delayEnabled = enabled;
            var ctx = Synth.getAudioContext();
            if (ctx) ctx.setDelayEnabled(enabled);
        },
        isDelayEnabled: function() { return delayEnabled; },

        setDelayTime: function(t) {
            delayTime = t;
            var ctx = Synth.getAudioContext();
            if (ctx) ctx.setDelayTime(t);
        },
        getDelayTime: function() { return delayTime; },

        setDelayFeedback: function(fb) {
            delayFeedback = fb;
            var ctx = Synth.getAudioContext();
            if (ctx) ctx.setDelayFeedback(fb);
        },
        getDelayFeedback: function() { return delayFeedback; },

        setDelayMix: function(m) {
            delayMix = m;
            var ctx = Synth.getAudioContext();
            if (ctx) ctx.setDelayMix(m);
        },
        getDelayMix: function() { return delayMix; },

        getState: function() {
            return {
                delayEnabled: delayEnabled,
                delayTime: delayTime,
                delayFeedback: delayFeedback,
                delayMix: delayMix
            };
        },

        loadState: function(state) {
            if (!state) return;
            Synth.Effects.setDelayTime(state.delayTime || 0.3);
            Synth.Effects.setDelayFeedback(state.delayFeedback || 0.3);
            Synth.Effects.setDelayMix(state.delayMix || 0.3);
            Synth.Effects.setDelayEnabled(state.delayEnabled || false);
        }
    };
})();
