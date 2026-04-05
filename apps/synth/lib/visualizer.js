(function() {
    'use strict';
    var Synth = window.Synth || (window.Synth = {});

    var canvas, ctx;
    var waveData = null;
    var frameCount = 0;
    var lastFpsTime = performance.now();
    var fps = 0;
    var micPitchCounter = 0;
    var paused = false;

    Synth.Visualizer = {
        init: function(canvasEl) {
            canvas = canvasEl;
            ctx = canvas.getContext('2d');
        },

        pause: function() {
            paused = true;
            if (ctx) {
                var W = ctx.canvasWidth;
                var H = ctx.canvasHeight;
                ctx.clearRect(0, 0, W, H);
            }
        },
        resume: function() {
            if (paused) { paused = false; Synth.Visualizer.draw(); }
        },

        draw: function() {
            if (paused) return;
            requestAnimationFrame(Synth.Visualizer.draw);

            frameCount++;
            var now = performance.now();
            if (now - lastFpsTime >= 1000) {
                fps = frameCount;
                frameCount = 0;
                lastFpsTime = now;
                document.getElementById('fps-display').textContent = fps + ' fps';
            }

            var W = ctx.canvasWidth;
            var H = ctx.canvasHeight;

            ctx.clearRect(0, 0, W, H);
            ctx.fillStyle = '#0a0a0f';
            ctx.fillRect(0, 0, W, H);

            updateMicInfo();

            // LFO modulation now runs natively on the audio thread (no JS tick needed)

            var analyser = Synth.getAnalyser();
            if (!analyser) return;

            if (!waveData || waveData.length !== analyser.fftSize) {
                waveData = new Float32Array(analyser.fftSize);
            }
            analyser.getFloatTimeDomainData(waveData);
            drawWaveform(W, H, waveData);
        }
    };

    function drawWaveform(W, H, data) {
        var bufLen = data.length;
        var midY = H / 2;

        ctx.strokeStyle = '#141420';
        ctx.lineWidth = 1;
        ctx.beginPath(); ctx.moveTo(0, midY); ctx.lineTo(W, midY); ctx.stroke();
        ctx.beginPath(); ctx.moveTo(0, midY * 0.5); ctx.lineTo(W, midY * 0.5); ctx.stroke();
        ctx.beginPath(); ctx.moveTo(0, midY * 1.5); ctx.lineTo(W, midY * 1.5); ctx.stroke();

        var rms = 0;
        for (var i = 0; i < bufLen; i++) rms += data[i] * data[i];
        rms = Math.sqrt(rms / bufLen);
        var intensity = Math.min(1.0, rms * 4);

        var triggerOffset = 0;
        var searchEnd = Math.floor(bufLen / 4);
        for (var i = 1; i < searchEnd; i++) {
            if (data[i - 1] <= 0 && data[i] > 0) { triggerOffset = i; break; }
        }

        var drawLen = Math.min(bufLen - triggerOffset, Math.floor(bufLen * 0.75));

        if (intensity > 0.01) {
            ctx.strokeStyle = 'rgba(0, 229, 255, ' + (intensity * 0.15) + ')';
            ctx.lineWidth = 10;
            ctx.beginPath();
            for (var i = 0; i < drawLen; i++) {
                var x = (i / drawLen) * W;
                var y = midY + data[triggerOffset + i] * midY * 0.85;
                if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
            }
            ctx.stroke();
        }

        if (intensity > 0.01) {
            ctx.strokeStyle = 'rgba(0, 229, 255, ' + (intensity * 0.3) + ')';
            ctx.lineWidth = 4;
            ctx.beginPath();
            for (var i = 0; i < drawLen; i++) {
                var x = (i / drawLen) * W;
                var y = midY + data[triggerOffset + i] * midY * 0.85;
                if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
            }
            ctx.stroke();
        }

        var r = Math.floor(intensity * 80);
        var g = Math.floor(200 + intensity * 55);
        ctx.strokeStyle = 'rgb(' + r + ', ' + g + ', 255)';
        ctx.lineWidth = 1.5;
        ctx.beginPath();
        for (var i = 0; i < drawLen; i++) {
            var x = (i / drawLen) * W;
            var y = midY + data[triggerOffset + i] * midY * 0.85;
            if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
        }
        ctx.stroke();

        if (intensity > 0.05) {
            ctx.fillStyle = 'rgba(255, 255, 255, ' + (intensity * 0.4) + ')';
            var prevY = midY, prevDy = 0;
            for (var i = 0; i < drawLen; i++) {
                var y = midY + data[triggerOffset + i] * midY * 0.85;
                var dy = y - prevY;
                if ((prevDy > 0 && dy <= 0 || prevDy < 0 && dy >= 0) && Math.abs(prevY - midY) > H * 0.15) {
                    var x = ((i - 1) / drawLen) * W;
                    ctx.beginPath(); ctx.arc(x, prevY, 2, 0, 6.283); ctx.fill();
                }
                prevDy = dy;
                prevY = y;
            }
        }
    }

    function updateMicInfo() {
        var micAnalyser = Synth.getMicAnalyser();
        var micLevelBuf = Synth.getMicLevelBuf();
        if (!micAnalyser || !micLevelBuf) return;

        micPitchCounter++;
        if (micPitchCounter % 6 !== 0) return;

        micAnalyser.getByteFrequencyData(micLevelBuf);
        var sum = 0, len = micLevelBuf.length;
        for (var i = 0; i < len; i += 4) sum += micLevelBuf[i];
        var avgLevel = sum / (len / 4) / 255;

        var bar = document.getElementById('mic-level-bar');
        bar.style.height = Math.min(100, avgLevel * 300) + '%';
        if (avgLevel > 0.6) bar.style.background = '#cc3333';
        else if (avgLevel > 0.3) bar.style.background = '#cccc33';
        else bar.style.background = '#33cc33';

        var micNoteEl = document.getElementById('mic-note');
        var micFreqEl = document.getElementById('mic-freq');

        if (!Synth.isMicEnabled()) {
            micNoteEl.textContent = 'Mic: --';
            micFreqEl.textContent = '-- Hz';
            return;
        }

        var freq = Synth.detectPitch();
        if (freq && freq > 50 && freq < 2000) {
            var info = Synth.freqToNoteName(freq);
            var c = info.cents >= 0 ? '+' + info.cents : '' + info.cents;
            micNoteEl.textContent = 'Mic: ' + info.name + ' (' + c + 'c)';
            micFreqEl.textContent = freq.toFixed(1) + ' Hz';
        } else {
            micNoteEl.textContent = 'Mic: --';
            micFreqEl.textContent = '-- Hz';
        }
    }
})();
