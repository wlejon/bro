// ---------------------------------------------------------------------------
// SignalChain — per-bus effect management (replaces effects.js + filter.js)
// ---------------------------------------------------------------------------

(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    function ctx() { return Synth.getAudioContext(); }

    Synth.SignalChain = {
        createBus: function() {
            var c = ctx();
            return c ? c.createBus() : -1;
        },

        destroyBus: function(busId) {
            var c = ctx();
            if (c && busId > 0) c.deleteBus(busId);
        },

        // Push all effect params from a layer/signal params object to a bus
        applyParams: function(busId, params) {
            if (busId < 0) return;
            var c = ctx();
            if (!c) return;
            var SC = Synth.SignalChain;

            // Effect chain order
            if (params.effectOrder) {
                SC.setEffectOrder(busId, params.effectOrder);
            }

            // Filter
            if (params.filter) {
                SC.setFilterType(busId, params.filter.type);
                SC.setFilterFrequency(busId, params.filter.frequency);
                SC.setFilterQ(busId, params.filter.Q);
                SC.setFilterGain(busId, params.filter.gain);
                SC.setFilterEnabled(busId, params.filter.enabled);
            }

            // Delay
            if (params.delay) {
                SC.setDelayTime(busId, params.delay.time);
                SC.setDelayFeedback(busId, params.delay.feedback);
                SC.setDelayMix(busId, params.delay.mix);
                SC.setDelayEnabled(busId, params.delay.enabled);
            }

            // Reverb
            if (params.reverb) {
                SC.setReverbRoomSize(busId, params.reverb.roomSize);
                SC.setReverbDamping(busId, params.reverb.damping);
                SC.setReverbMix(busId, params.reverb.mix);
                SC.setReverbEnabled(busId, params.reverb.enabled);
            }

            // Chorus
            if (params.chorus) {
                SC.setChorusRate(busId, params.chorus.rate);
                SC.setChorusDepth(busId, params.chorus.depth);
                SC.setChorusMix(busId, params.chorus.mix);
                SC.setChorusFeedback(busId, params.chorus.feedback);
                SC.setChorusBaseDelay(busId, params.chorus.baseDelay);
                SC.setChorusEnabled(busId, params.chorus.enabled);
            }

            // Compressor
            if (params.compressor) {
                SC.setCompressorThreshold(busId, params.compressor.threshold);
                SC.setCompressorRatio(busId, params.compressor.ratio);
                SC.setCompressorAttack(busId, params.compressor.attack);
                SC.setCompressorRelease(busId, params.compressor.release);
                SC.setCompressorEnabled(busId, params.compressor.enabled);
                if (params.compressor.sidechainBusId >= 0) {
                    SC.setCompressorSidechain(busId, params.compressor.sidechainBusId);
                }
            }

            // Equalizer
            if (params.eq) {
                SC.setEqMasterGain(busId, params.eq.masterGain);
                for (var i = 0; i < params.eq.bands.length; i++) {
                    SC.setEqBandGain(busId, i, params.eq.bands[i]);
                }
                SC.setEqEnabled(busId, params.eq.enabled);
            }

            // Distortion
            if (params.distortion) {
                SC.setDistortionMode(busId, params.distortion.mode);
                SC.setDistortionDrive(busId, params.distortion.drive);
                SC.setDistortionMix(busId, params.distortion.mix);
                SC.setDistortionOutputGain(busId, params.distortion.outputGain);
                SC.setDistortionCrushBits(busId, params.distortion.crushBits);
                SC.setDistortionCrushRate(busId, params.distortion.crushRate);
                SC.setDistortionEnabled(busId, params.distortion.enabled);
            }
        },

        // --- Effect chain order ---

        setEffectOrder: function(busId, order) {
            var c = ctx(); if (c) c.setBusEffectOrder(busId, order);
        },

        // --- Filter (uses bus filter slot 0) ---

        setFilterEnabled: function(busId, v) {
            var c = ctx(); if (c) c.setBusFilterEnabled(busId, 0, v);
        },
        setFilterType: function(busId, type) {
            var c = ctx(); if (c) c.setBusFilterType(busId, 0, type || 'lowpass');
        },
        setFilterFrequency: function(busId, freq) {
            var c = ctx(); if (c) c.setBusFilterFrequency(busId, 0, freq);
        },
        setFilterQ: function(busId, q) {
            var c = ctx(); if (c) c.setBusFilterQ(busId, 0, q);
        },
        setFilterGain: function(busId, g) {
            var c = ctx(); if (c) c.setBusFilterGain(busId, 0, g);
        },

        // --- Delay ---

        setDelayEnabled: function(busId, v) {
            var c = ctx(); if (c) c.setBusDelayEnabled(busId, v);
        },
        setDelayTime: function(busId, v) {
            var c = ctx(); if (c) c.setBusDelayTime(busId, v);
        },
        setDelayFeedback: function(busId, v) {
            var c = ctx(); if (c) c.setBusDelayFeedback(busId, v);
        },
        setDelayMix: function(busId, v) {
            var c = ctx(); if (c) c.setBusDelayMix(busId, v);
        },

        // --- Reverb ---

        setReverbEnabled: function(busId, v) {
            var c = ctx(); if (c) c.setBusReverbEnabled(busId, v);
        },
        setReverbRoomSize: function(busId, v) {
            var c = ctx(); if (c) c.setBusReverbRoomSize(busId, v);
        },
        setReverbDamping: function(busId, v) {
            var c = ctx(); if (c) c.setBusReverbDamping(busId, v);
        },
        setReverbMix: function(busId, v) {
            var c = ctx(); if (c) c.setBusReverbMix(busId, v);
        },

        // --- Chorus ---

        setChorusEnabled: function(busId, v) {
            var c = ctx(); if (c) c.setBusChorusEnabled(busId, v);
        },
        setChorusRate: function(busId, v) {
            var c = ctx(); if (c) c.setBusChorusRate(busId, v);
        },
        setChorusDepth: function(busId, v) {
            var c = ctx(); if (c) c.setBusChorusDepth(busId, v);
        },
        setChorusMix: function(busId, v) {
            var c = ctx(); if (c) c.setBusChorusMix(busId, v);
        },
        setChorusFeedback: function(busId, v) {
            var c = ctx(); if (c) c.setBusChorusFeedback(busId, v);
        },
        setChorusBaseDelay: function(busId, v) {
            var c = ctx(); if (c) c.setBusChorusBaseDelay(busId, v);
        },

        // --- Compressor ---

        setCompressorEnabled: function(busId, v) {
            var c = ctx(); if (c) c.setBusCompressorEnabled(busId, v);
        },
        setCompressorThreshold: function(busId, v) {
            var c = ctx(); if (c) c.setBusCompressorThreshold(busId, v);
        },
        setCompressorRatio: function(busId, v) {
            var c = ctx(); if (c) c.setBusCompressorRatio(busId, v);
        },
        setCompressorAttack: function(busId, v) {
            var c = ctx(); if (c) c.setBusCompressorAttack(busId, v);
        },
        setCompressorRelease: function(busId, v) {
            var c = ctx(); if (c) c.setBusCompressorRelease(busId, v);
        },
        setCompressorSidechain: function(busId, scBusId) {
            var c = ctx(); if (c) c.setBusCompressorSidechain(busId, scBusId);
        },

        // --- Equalizer ---

        setEqEnabled: function(busId, v) {
            var c = ctx(); if (c) c.setBusEqEnabled(busId, v);
        },
        setEqBandGain: function(busId, band, gainDB) {
            var c = ctx(); if (c) c.setBusEqBandGain(busId, band, gainDB);
        },
        setEqMasterGain: function(busId, gainDB) {
            var c = ctx(); if (c) c.setBusEqMasterGain(busId, gainDB);
        },

        // --- Distortion ---

        setDistortionEnabled: function(busId, v) {
            var c = ctx(); if (c) c.setBusDistortionEnabled(busId, v);
        },
        setDistortionMode: function(busId, mode) {
            var c = ctx(); if (c) c.setBusDistortionMode(busId, mode || 'softclip');
        },
        setDistortionDrive: function(busId, v) {
            var c = ctx(); if (c) c.setBusDistortionDrive(busId, v);
        },
        setDistortionMix: function(busId, v) {
            var c = ctx(); if (c) c.setBusDistortionMix(busId, v);
        },
        setDistortionOutputGain: function(busId, v) {
            var c = ctx(); if (c) c.setBusDistortionOutputGain(busId, v);
        },
        setDistortionCrushBits: function(busId, v) {
            var c = ctx(); if (c) c.setBusDistortionCrushBits(busId, v);
        },
        setDistortionCrushRate: function(busId, v) {
            var c = ctx(); if (c) c.setBusDistortionCrushRate(busId, v);
        },

        // --- Metering ---

        getBusPeakL: function(busId) {
            var c = ctx(); return c ? c.getBusPeakL(busId) : 0;
        },
        getBusPeakR: function(busId) {
            var c = ctx(); return c ? c.getBusPeakR(busId) : 0;
        },
        getBusRmsL: function(busId) {
            var c = ctx(); return c ? c.getBusRmsL(busId) : 0;
        },
        getBusRmsR: function(busId) {
            var c = ctx(); return c ? c.getBusRmsR(busId) : 0;
        }
    };
})();
