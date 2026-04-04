(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var enabled = false;
    var rate = 5.0;    // Hz
    var depth = 0.5;   // 0..1
    var waveform = 'sine';
    var target = 'pitch'; // pitch, filter, volume
    var phase = 0;

    function computeWave(p) {
        switch (waveform) {
            case 'sine': return Math.sin(p * 2 * Math.PI);
            case 'triangle': return p < 0.5 ? (4 * p - 1) : (3 - 4 * p);
            case 'square': return p < 0.5 ? 1 : -1;
            case 'sawtooth': return 2 * p - 1;
            default: return Math.sin(p * 2 * Math.PI);
        }
    }

    Synth.LFO = {
        update: function(dt) {
            if (!enabled) return;
            phase += rate * dt;
            if (phase >= 1) phase -= Math.floor(phase);

            var value = computeWave(phase) * depth;

            if (target === 'pitch') {
                // Modulate pitch of active notes: +/- semitones based on depth
                var notes = Synth.getActiveNotes();
                notes.forEach(function(entry) {
                    if (entry.osc) {
                        var mod = entry.baseFreq * Math.pow(2, value / 12);
                        entry.osc.frequency.value = mod;
                    }
                });
            } else if (target === 'filter') {
                if (Synth.Filter && Synth.Filter.isEnabled()) {
                    Synth.Filter.modulateCutoff(value);
                }
            } else if (target === 'volume') {
                // Tremolo: modulate master gain 0.5..1.5 range
                var mg = Synth.getMasterGain();
                if (mg) mg.gain.value = 1.0 + value * 0.5;
            } else if (target === 'pan') {
                // Auto-pan: sweep stereo position left-right
                var notes = Synth.getActiveNotes();
                notes.forEach(function(entry) {
                    if (entry.osc) {
                        entry.osc.pan.value = value;
                    }
                });
            }
        },

        setEnabled: function(e) { enabled = e; if (!e) { phase = 0; resetTargets(); } },
        isEnabled: function() { return enabled; },

        setRate: function(r) { rate = r; },
        getRate: function() { return rate; },

        setDepth: function(d) { depth = d; },
        getDepth: function() { return depth; },

        setWaveform: function(wf) { waveform = wf; },
        getWaveform: function() { return waveform; },

        setTarget: function(t) { resetTargets(); target = t; },
        getTarget: function() { return target; },

        getState: function() {
            return { enabled: enabled, rate: rate, depth: depth, waveform: waveform, target: target };
        },

        loadState: function(state) {
            if (!state) return;
            rate = state.rate || 5.0;
            depth = state.depth || 0.5;
            waveform = state.waveform || 'sine';
            target = state.target || 'pitch';
            enabled = state.enabled || false;
        }
    };

    function resetTargets() {
        // Reset modulated values to base
        var notes = Synth.getActiveNotes();
        notes.forEach(function(entry) {
            if (entry.osc) {
                entry.osc.frequency.value = entry.baseFreq;
                entry.osc.pan.value = Synth.voicePan || 0;
            }
        });
        var mg = Synth.getMasterGain();
        if (mg) mg.gain.value = 1.0;
    }
})();
