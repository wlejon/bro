// ---------------------------------------------------------------------------
// LFO — delegates to native ModMatrix (sample-accurate, per-voice)
// ---------------------------------------------------------------------------

(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var modMatrix = null;
    var enabled = false;
    var rate = 5.0;
    var depth = 0.5;
    var waveform = 'sine';
    var target = 'pitch';
    var sync = false;
    var routeIndex = -1;

    var LFO_INDEX = 0; // use LFO 1 for the main synth LFO

    function targetToDest(t) {
        switch (t) {
            case 'filter': return 'filterfreq';
            case 'volume': return 'gain';
            case 'pan': return 'pan';
            default: return 'pitch';
        }
    }

    function lfoShapeFromWaveform(wf) {
        switch (wf) {
            case 'triangle': return 'triangle';
            case 'square': return 'square';
            case 'sawtooth': return 'sawup';
            case 'samplehold': return 'samplehold';
            default: return 'sine';
        }
    }

    function applyRoute() {
        if (!modMatrix) return;
        // Remove old route
        if (routeIndex >= 0) {
            modMatrix.removeRoute(routeIndex);
            routeIndex = -1;
        }
        if (enabled) {
            routeIndex = modMatrix.addRoute('lfo1', targetToDest(target), depth);
        }
    }

    Synth.LFO = {
        init: function(ctx) {
            modMatrix = ctx.getModMatrix();
        },

        setEnabled: function(e) {
            enabled = e;
            applyRoute();
        },
        isEnabled: function() { return enabled; },

        setRate: function(r) {
            rate = r;
            if (modMatrix) modMatrix.setLfoRate(LFO_INDEX, r);
        },
        getRate: function() { return rate; },

        setDepth: function(d) {
            depth = d;
            if (modMatrix && routeIndex >= 0) {
                modMatrix.setRouteAmount(routeIndex, d);
            }
        },
        getDepth: function() { return depth; },

        setWaveform: function(wf) {
            waveform = wf;
            if (modMatrix) modMatrix.setLfoShape(LFO_INDEX, lfoShapeFromWaveform(wf));
        },
        getWaveform: function() { return waveform; },

        setTarget: function(t) {
            target = t;
            applyRoute();
        },
        getTarget: function() { return target; },

        setSync: function(s) {
            sync = s;
            if (modMatrix) modMatrix.setLfoSync(LFO_INDEX, s);
        },
        isSync: function() { return sync; },

        getState: function() {
            return { enabled: enabled, rate: rate, depth: depth, waveform: waveform,
                     target: target, sync: sync };
        },

        loadState: function(state) {
            if (!state) return;
            rate = state.rate || 5.0;
            depth = state.depth || 0.5;
            waveform = state.waveform || 'sine';
            target = state.target || 'pitch';
            sync = state.sync || false;
            enabled = state.enabled || false;

            if (modMatrix) {
                modMatrix.setLfoRate(LFO_INDEX, rate);
                modMatrix.setLfoShape(LFO_INDEX, lfoShapeFromWaveform(waveform));
                modMatrix.setLfoSync(LFO_INDEX, sync);
            }
            applyRoute();
        }
    };
})();
