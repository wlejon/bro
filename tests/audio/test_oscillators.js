// Oscillator waveforms — verify each waveform produces non-silent output,
// and pan affects stereo output (measured via bus L/R meters).

const ctx = new AudioContext();

function rmsOf(buf) {
    let s = 0;
    for (let i = 0; i < buf.length; i++) s += buf[i] * buf[i];
    return Math.sqrt(s / buf.length);
}

function recordWaveform(type) {
    const osc = ctx.createOscillator();
    osc.type = type;
    osc.frequency.value = 440;
    osc.connect(ctx.destination);
    ctx.startRecording();
    osc.start();
    sleep(150);
    osc.stop();
    sleep(30);
    return ctx.stopRecording();
}

const waveforms = ['sine', 'square', 'sawtooth', 'triangle', 'whitenoise', 'pinknoise', 'brownnoise'];
for (const w of waveforms) {
    const buf = recordWaveform(w);
    assert(buf && buf.length > 0, w + ' produced recording buffer');
    const rms = rmsOf(buf);
    console.log(w, 'rms:', rms, 'len:', buf.length);
    assert(rms > 0.001, w + ' is non-silent (rms=' + rms + ')');
}

// Frequency setter round-trip
{
    const osc = ctx.createOscillator();
    osc.frequency.value = 880;
    assert(Math.abs(osc.frequency.value - 880) < 1e-3, 'frequency round-trips, got ' + osc.frequency.value);
}

// Pan via bus L/R metering (recording is mono mixdown so can't see L/R there).
function panPeaks(panVal) {
    const v = ctx.createVoice();
    ctx.setVoiceWaveform(v, 'sine');
    ctx.setVoiceFrequency(v, 440);
    ctx.setVoiceGain(v, 0.5);
    ctx.setVoiceAttack(v, 0.005);
    ctx.setVoiceSustain(v, 1.0);
    ctx.setVoiceRelease(v, 0.005);
    ctx.setVoicePan(v, panVal);
    ctx.startVoice(v, ctx.currentTime);
    sleep(120);
    const l = ctx.getBusPeakL(0);
    const r = ctx.getBusPeakR(0);
    ctx.stopVoice(v, ctx.currentTime);
    sleep(20);
    ctx.removeVoice(v);
    return { l, r };
}

const left  = panPeaks(-1.0);
const right = panPeaks( 1.0);
console.log('pan=-1 L:', left.l, 'R:', left.r);
console.log('pan=+1 L:', right.l, 'R:', right.r);
assert(left.l > left.r * 5, 'pan=-1 makes L >> R (L=' + left.l + ' R=' + left.r + ')');
assert(right.r > right.l * 5, 'pan=+1 makes R >> L (L=' + right.l + ' R=' + right.r + ')');
