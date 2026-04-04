// ---------------------------------------------------------------------------
// WAV file parser / encoder (PCM only)
// ---------------------------------------------------------------------------
(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var WAV = Synth.WAV = {};

    // Decode a WAV file (ArrayBuffer) to mono Float32Array at native sample rate
    WAV.decode = function(arrayBuffer) {
        var view = new DataView(arrayBuffer);

        // RIFF header
        var riff = String.fromCharCode(view.getUint8(0), view.getUint8(1), view.getUint8(2), view.getUint8(3));
        if (riff !== 'RIFF') throw new Error('Not a WAV file');

        var wave = String.fromCharCode(view.getUint8(8), view.getUint8(9), view.getUint8(10), view.getUint8(11));
        if (wave !== 'WAVE') throw new Error('Not a WAVE file');

        // Find fmt and data chunks
        var offset = 12;
        var fmt = null;
        var dataChunk = null;

        while (offset < view.byteLength - 8) {
            var chunkId = String.fromCharCode(
                view.getUint8(offset), view.getUint8(offset + 1),
                view.getUint8(offset + 2), view.getUint8(offset + 3)
            );
            var chunkSize = view.getUint32(offset + 4, true);

            if (chunkId === 'fmt ') {
                fmt = {
                    audioFormat: view.getUint16(offset + 8, true),
                    numChannels: view.getUint16(offset + 10, true),
                    sampleRate: view.getUint32(offset + 12, true),
                    bitsPerSample: view.getUint16(offset + 22, true)
                };
            } else if (chunkId === 'data') {
                dataChunk = { offset: offset + 8, size: chunkSize };
            }

            offset += 8 + chunkSize;
            if (chunkSize % 2 !== 0) offset++; // pad byte
        }

        if (!fmt) throw new Error('No fmt chunk found');
        if (!dataChunk) throw new Error('No data chunk found');
        if (fmt.audioFormat !== 1 && fmt.audioFormat !== 3) {
            throw new Error('Unsupported format (only PCM int/float)');
        }

        var bytesPerSample = fmt.bitsPerSample / 8;
        var numSamples = Math.floor(dataChunk.size / (bytesPerSample * fmt.numChannels));
        var samples = new Float32Array(numSamples);
        var dOff = dataChunk.offset;

        for (var i = 0; i < numSamples; i++) {
            var sum = 0;
            for (var ch = 0; ch < fmt.numChannels; ch++) {
                var pos = dOff + (i * fmt.numChannels + ch) * bytesPerSample;
                if (fmt.audioFormat === 3) {
                    // IEEE float
                    sum += bytesPerSample === 4 ? view.getFloat32(pos, true) : view.getFloat64(pos, true);
                } else if (fmt.bitsPerSample === 16) {
                    sum += view.getInt16(pos, true) / 32768;
                } else if (fmt.bitsPerSample === 24) {
                    var s = view.getUint8(pos) | (view.getUint8(pos + 1) << 8) | (view.getUint8(pos + 2) << 16);
                    if (s & 0x800000) s |= ~0xFFFFFF; // sign extend
                    sum += s / 8388608;
                } else if (fmt.bitsPerSample === 8) {
                    sum += (view.getUint8(pos) - 128) / 128;
                } else if (fmt.bitsPerSample === 32) {
                    sum += view.getInt32(pos, true) / 2147483648;
                }
            }
            samples[i] = sum / fmt.numChannels; // mix to mono
        }

        return { samples: samples, sampleRate: fmt.sampleRate, channels: fmt.numChannels };
    };

    // Resample to target sample rate using linear interpolation
    WAV.resample = function(samples, fromRate, toRate) {
        if (fromRate === toRate) return samples;
        var ratio = fromRate / toRate;
        var outLen = Math.floor(samples.length / ratio);
        var out = new Float32Array(outLen);
        for (var i = 0; i < outLen; i++) {
            var srcPos = i * ratio;
            var idx = Math.floor(srcPos);
            var frac = srcPos - idx;
            var s0 = samples[idx];
            var s1 = idx + 1 < samples.length ? samples[idx + 1] : s0;
            out[i] = s0 + frac * (s1 - s0);
        }
        return out;
    };

    // Encode Float32Array to WAV ArrayBuffer (16-bit PCM, mono, 44100Hz)
    WAV.encode = function(samples, sampleRate) {
        sampleRate = sampleRate || 44100;
        var numSamples = samples.length;
        var bytesPerSample = 2; // 16-bit
        var dataSize = numSamples * bytesPerSample;
        var buffer = new ArrayBuffer(44 + dataSize);
        var view = new DataView(buffer);

        // RIFF header
        writeString(view, 0, 'RIFF');
        view.setUint32(4, 36 + dataSize, true);
        writeString(view, 8, 'WAVE');

        // fmt chunk
        writeString(view, 12, 'fmt ');
        view.setUint32(16, 16, true);           // chunk size
        view.setUint16(20, 1, true);            // PCM
        view.setUint16(22, 1, true);            // mono
        view.setUint32(24, sampleRate, true);
        view.setUint32(28, sampleRate * bytesPerSample, true); // byte rate
        view.setUint16(32, bytesPerSample, true); // block align
        view.setUint16(34, 16, true);           // bits per sample

        // data chunk
        writeString(view, 36, 'data');
        view.setUint32(40, dataSize, true);

        for (var i = 0; i < numSamples; i++) {
            var s = Math.max(-1, Math.min(1, samples[i]));
            view.setInt16(44 + i * 2, s < 0 ? s * 32768 : s * 32767, true);
        }

        return buffer;
    };

    function writeString(view, offset, str) {
        for (var i = 0; i < str.length; i++) {
            view.setUint8(offset + i, str.charCodeAt(i));
        }
    }
})();
