// FFT spectrum analysis — feed a sine, peak bin should be near the input frequency.

const ctx = new AudioContext();
const sr = ctx.sampleRate;

// ctx.getSpectrum is the global API
{
    const osc = ctx.createOscillator();
    osc.type = 'sine';
    osc.frequency.value = 1000;
    osc.connect(ctx.destination);
    osc.start();
    sleep(250);   // let FFT accumulate
    const bins = 1024;
    const spec = ctx.getSpectrum(bins);
    console.log('spec type:', spec && spec.constructor.name, 'len:', spec && spec.length);
    assert(spec && spec.length === bins, 'getSpectrum returns Float32Array length=' + bins + ', got ' + (spec && spec.length));

    // Find peak bin
    let maxBin = 0, maxVal = -Infinity;
    for (let i = 0; i < bins; i++) {
        if (spec[i] > maxVal) { maxVal = spec[i]; maxBin = i; }
    }
    // Docs say "magnitude data" but don't specify whether bins span [0,sr/2]
    // (Web Audio convention) or [0,sr] (FFT-bin convention).
    const hzNyquist = maxBin * (sr / 2) / bins;
    const hzFull    = maxBin * sr / bins;
    console.log('peak bin:', maxBin, 'hzNyquist:', hzNyquist, 'hzFull:', hzFull, 'maxVal:', maxVal);
    // Accept either convention but flag the deviation: Web Audio convention says hzNyquist.
    assert(Math.abs(hzNyquist - 1000) < 100,
           'spectrum peak near 1000Hz under Web Audio Nyquist convention, got ' + hzNyquist + 'Hz; under full-rate convention it is ' + hzFull + 'Hz');
    osc.stop();
}

// AnalyserNode API
{
    const an = ctx.createAnalyser();
    assert(an, 'createAnalyser returns node');
    an.fftSize = 1024;
    console.log('fftSize:', an.fftSize, 'binCount:', an.frequencyBinCount);
    assert(an.fftSize === 1024, 'fftSize round-trips, got ' + an.fftSize);
    assert(an.frequencyBinCount === 512, 'frequencyBinCount = fftSize/2, got ' + an.frequencyBinCount);

    const osc = ctx.createOscillator();
    osc.type = 'sine';
    osc.frequency.value = 2000;
    osc.connect(an);
    osc.start();
    sleep(250);
    const fd = new Float32Array(an.frequencyBinCount);
    an.getFloatFrequencyData(fd);
    let peakI = 0, peakV = -Infinity;
    for (let i = 0; i < fd.length; i++) if (fd[i] > peakV) { peakV = fd[i]; peakI = i; }
    const hzNyq = peakI * (sr / 2) / an.frequencyBinCount;
    const hzFul = peakI * sr / an.frequencyBinCount;
    console.log('analyser peak bin:', peakI, 'hzNyq:', hzNyq, 'hzFull:', hzFul, 'val:', peakV);
    assert(Math.abs(hzNyq - 2000) < 300, 'analyser peak near 2000Hz under Web Audio convention, got ' + hzNyq + ' (full-rate would be ' + hzFul + ')');
    osc.stop();
}
