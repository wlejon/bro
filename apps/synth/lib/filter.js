(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var filterNode = null;
    var filterEnabled = false;
    var filterType = 'lowpass';
    var filterFreq = 1000;
    var filterQ = 1.0;
    var filterGain = 0;
    var baseCutoff = 1000; // before LFO modulation

    Synth.Filter = {
        init: function() {
            var ctx = Synth.getAudioContext();
            if (!ctx) return;
            filterNode = ctx.createBiquadFilter();
            filterNode.type = filterType;
            filterNode.frequency.value = filterFreq;
            filterNode.Q.value = filterQ;
        },

        setEnabled: function(enabled) {
            filterEnabled = enabled;
            if (filterNode) {
                if (enabled) {
                    filterNode.connect(Synth.getAudioContext().destination);
                } else {
                    filterNode.disconnect();
                }
            }
        },
        isEnabled: function() { return filterEnabled; },

        setType: function(type) {
            filterType = type;
            if (filterNode) filterNode.type = type;
        },
        getType: function() { return filterType; },

        setCutoff: function(freq) {
            filterFreq = freq;
            baseCutoff = freq;
            if (filterNode) filterNode.frequency.value = freq;
        },
        getCutoff: function() { return filterFreq; },

        setQ: function(q) {
            filterQ = q;
            if (filterNode) filterNode.Q.value = q;
        },
        getQ: function() { return filterQ; },

        setGain: function(g) {
            filterGain = g;
            if (filterNode) filterNode.gain.value = g;
        },

        // LFO modulation applies an offset to the base cutoff
        modulateCutoff: function(offset) {
            // offset is -1..1, map to frequency multiplier
            var mult = Math.pow(2, offset * 2); // +/- 2 octaves
            var freq = Math.max(20, Math.min(20000, baseCutoff * mult));
            filterFreq = freq;
            if (filterNode) filterNode.frequency.value = freq;
        },

        getState: function() {
            return {
                enabled: filterEnabled,
                type: filterType,
                frequency: baseCutoff,
                Q: filterQ,
                gain: filterGain
            };
        },

        loadState: function(state) {
            if (!state) return;
            Synth.Filter.setType(state.type || 'lowpass');
            Synth.Filter.setCutoff(state.frequency || 1000);
            Synth.Filter.setQ(state.Q || 1.0);
            Synth.Filter.setGain(state.gain || 0);
            Synth.Filter.setEnabled(state.enabled || false);
        }
    };
})();
