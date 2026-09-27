/**
 * =============================================================================
 * bro.ear — judge how a clip sounds, as numbers and pictures
 * =============================================================================
 *
 * Offline analysis of a finished clip, so a script (or an agent) can decide
 * whether one variant sounds better than another without listening:
 *
 *   bro.ear.measure(clip, opts?)          a report: timing, loudness, spectrum,
 *                                         tonality, and the pure partials that
 *                                         ring (the "sounds synthetic" signature)
 *   bro.ear.compare(clip, reference, opts?)  distance from a reference recording,
 *                                         overall and per component; lower = closer
 *   bro.ear.spectrogram(clipOrClips, opts?)  an RGBA image with labelled axes,
 *                                         several clips on one time and dB scale
 *   bro.ear.fit(graph, opts)              search a SynthGraph's parameters until
 *                                         its render meets a target (a reference,
 *                                         measurements, a scorer, CLAP prompts)
 *   bro.ear.loadClap(dir?)                the CLAP text-prompt scorer (ML builds
 *                                         only; see the section at the end)
 *
 * measure / compare / spectrogram / fit live in broaudio
 * (include/broaudio/ear/ear.h and ear/fit.h hold the C++ contract) and are
 * present in every build profile, including minimal, and in Worker realms. They run synchronously on the calling thread,
 * need no AudioContext and no audio device, and are DETERMINISTIC: the same
 * input gives the same numbers and the same pixels, so a loop of "generate a
 * variant, score it, keep the better one" is reproducible headless.
 *
 * CLIPS. Every `clip` argument is any of
 *   - a path string ("sfx/hit.wav"): decoded with the same loaders as
 *     AudioContext.decodeAudioFile (wav / flac / mp3 / ogg / opus), resolved
 *     like fs.* paths (relative to the app);
 *   - an AudioBuffer;
 *   - {samples: Float32Array, sampleRate, channels?}: interleaved samples,
 *     which is what decodeAudioFile and the bro.tts synthesizers return;
 *   - a bare Float32Array (mono), with `opts.sampleRate` giving its rate.
 * Multichannel clips are averaged to mono before analysis.
 *
 * UNITS. Times are seconds; levels are dBFS (0 dB = a full-scale sample for
 * peaks, the RMS of a full-scale square wave for RMS, so a full-scale sine is
 * 0 dB peak / -3 dB RMS; silence floors at -120); frequencies are Hz. A value
 * that does not exist for a clip (t60 of a sound that does not decay, LUFS of
 * silence) is null.
 *
 * @example
 *   // Keep the variant that is closest to a real recording.
 *   const ref = 'refs/marimba_c5.wav';
 *   let best = null;
 *   for (let i = 0; i < 20; i++) {
 *     const clip = renderVariant(i);                   // {samples, sampleRate}
 *     const d = bro.ear.compare(clip, ref);
 *     if (!best || d.score < best.d.score) best = { i, d, clip };
 *   }
 *   console.log('best', best.i, JSON.stringify(best.d));
 *   bro.ear.spectrogram([best.clip, ref], { labels: ['best', 'reference'],
 *                                           path: 'out/best_vs_ref.png' });
 *
 * @example
 *   // Is it ringing like a sine bell?
 *   const m = bro.ear.measure(clip);
 *   if (m.ringing.ringScore > 0.6)
 *     console.log('a few pure partials ring for', m.ringing.weightedRingTime.toFixed(2), 's',
 *                 'inharmonicity', m.ringing.inharmonicity.toFixed(2));
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * @typedef {string|AudioBuffer|Float32Array|{samples: Float32Array, sampleRate: number, channels?: number}} EarClip
 */

/**
 * @typedef {Object} EarMeasureOptions
 * @property {number} [sampleRate]      rate of a bare Float32Array clip
 * @property {number} [tailFloorDb=-60] the tail ends where the envelope stays below
 *                                      (envelope peak + tailFloorDb), or 6 dB over the
 *                                      noise floor, whichever is higher
 * @property {number} [maxPartials=8]   how many partials to list (strongest first)
 * @property {number} [slices=8]        how many equal slices the timeline has
 */

/**
 * One slice of the coarse timeline.
 * @typedef {Object} EarSlice
 * @property {number} time        slice start (s)
 * @property {number} duration
 * @property {number} rmsDb
 * @property {number} centroidHz  of the slice's average power spectrum
 * @property {number} flatness    ditto: 0 = pure tone .. ~1 = white noise
 * @property {number} tonality    tonal share of the slice's energy
 */

/**
 * A tracked sinusoidal partial: a spectral peak at least 15 dB over the
 * local median spectrum, continuous in frequency (within max(1 bin, 2.5 %))
 * from frame to frame for at least 60 ms (~100 ms analysis window). A
 * partial that stops and starts again (a repeated note) is two entries.
 * @typedef {Object} EarPartial
 * @property {number} freqHz          energy-weighted mean frequency
 * @property {number} peakDb          its loudest amplitude, dBFS (a sine of amplitude A reads
 *                                    20·log10 A; a fast decay reads up to ~1.5 dB low, the
 *                                    analysis window averaging over it)
 * @property {number} startTime
 * @property {number} peakTime
 * @property {number} endTime         last frame it is tracked in
 * @property {number} ringTime        peakTime to the last frame within 60 dB of peakDb
 * @property {number} decayRate       dB per second after the peak (least squares; 0 = steady)
 * @property {number|null} t60        60 / decayRate; null when it does not decay
 * @property {number} stabilityCents  energy-weighted frequency deviation
 * @property {number} energyShare     its energy / the whole clip's energy
 * @property {number|null} ratio      freqHz / ringing.f0Hz
 */

/**
 * The "a few pure tones ring on" summary. ringScore near 1 is a sparse set
 * of long-ringing, pure partials; with inharmonicity well above 0 as well
 * (partials that are not a harmonic series, like a free bar's 1 : 2.76 :
 * 5.40) it is the signature of a sound that reads as synthetic: a sine bell,
 * a modal xylophone model. Real acoustic hits usually spread their energy
 * over more partials and noise, so their sparsity and tonality are lower.
 * @typedef {Object} EarRinging
 * @property {number} count              distinct partial frequencies within 30 dB of the strongest
 * @property {number} sparsity           energy of the 3 strongest partial frequencies / all tonal energy
 * @property {number} strongestRingTime  ringTime of the most energetic partial
 * @property {number} weightedRingTime   energy-weighted mean ringTime of the counted partials
 * @property {number|null} f0Hz          the fundamental that best explains them as a harmonic series
 * @property {number} inharmonicity      0 = exact harmonics of f0Hz .. 1 = unrelated. Twice the
 *                                       amplitude-weighted mean distance of each partial's harmonic
 *                                       number from an integer; f0 candidates are f/k for the 3
 *                                       strongest partials and k = 1..6, costed at that distance
 *                                       + 0.05 per step of k
 * @property {number} ringScore          tonality × sparsity × min(1, weightedRingTime / 0.5 s)
 */

/**
 * The measure() report. The envelope is the RMS of 10 ms windows every 5 ms.
 * @typedef {Object} EarMeasurement
 * @property {number} sampleRate       the clip's own rate
 * @property {number} channels         before the mono mixdown
 * @property {number} duration
 * @property {number} peakTime         time of the largest |sample|
 * @property {number} envelopePeakTime time of the loudest envelope frame
 * @property {number} onsetTime        first envelope frame within 30 dB of the envelope peak
 * @property {number} attackTime       envelopePeakTime - onsetTime
 * @property {number} tailTime         envelopePeakTime to where the envelope stays below the
 *                                     tail threshold (see tailFloorDb)
 * @property {'floor'|'noise'|'end'} tailEnd  what ended the tail: the tailFloorDb level, the
 *                                     noise floor, or the end of the clip (still ringing)
 * @property {number|null} noiseFloorDb  10th percentile of the envelope, when it is at least
 *                                     20 dB under the peak and a plateau (the frames after the
 *                                     peak within 3 dB of it fall slower than 3 dB/s)
 * @property {number} decayRate        dB/s: slope of the Schroeder energy-decay curve from
 *                                     -5 dB to -35 dB (or 60 % of the tail's depth, if that is
 *                                     at least 15 dB); 0 when there is no such span
 * @property {number|null} t60         60 / decayRate (extrapolated reverberation-style T60)
 * @property {number} peakDb           sample peak
 * @property {number} envelopePeakDb   loudest 10 ms RMS
 * @property {number} rmsDb            whole-clip RMS
 * @property {number|null} lufs        ITU-R BS.1770-4 integrated loudness of the mono clip:
 *                                     K-weighted, 400 ms blocks at 75 % overlap, -70 LUFS
 *                                     absolute and -10 LU relative gates (one block when the
 *                                     clip is shorter than 400 ms). A full-scale 1 kHz sine
 *                                     reads -3.01. null when nothing passes the absolute gate.
 *                                     Under 400 ms its one block is the clip's energy over the
 *                                     clip's own length, so silence around the sound lowers
 *                                     it: judge short clips by lufsShort / loudness
 * @property {number|null} lufsShort   short-clip loudness, meaningful at any length: the
 *                                     loudest 100 ms window of the K-weighted signal (a clip
 *                                     under 100 ms counts as zero-padded to one window), LUFS
 *                                     units. Independent of silence around the sound, exactly
 *                                     +6.02 dB per doubling of level, a steady sound reads its
 *                                     lufs (the 1 kHz sine -3.01 even at 300 ms), and clicks
 *                                     shorter than 100 ms read by their energy (twice as long,
 *                                     ~+3 dB), as the ear integrates them. null under -70
 * @property {number|null} loudness    the loudness the ear judges by: lufsShort under 400 ms,
 *                                     lufs from 800 ms, and between them a blend in dB weighted
 *                                     by the duration (continuous in the clip's length).
 *                                     compare() normalises by it and fit() can target it
 * @property {number} centroidHz       spectral centroid of the long-term average power
 *                                     spectrum (frames within 60 dB of the loudest)
 * @property {number} flatness         spectral flatness (geometric / arithmetic mean power,
 *                                     30 Hz .. min(16 kHz, Nyquist)) of the same spectrum:
 *                                     ~0 for a tone, ~0.9+ for white noise
 * @property {number} tonality         share of the clip's energy in tracked partials (0..1)
 * @property {EarSlice[]} timeline     `slices` equal slices over the clip
 * @property {EarPartial[]} partials   strongest (by energy) first, at most maxPartials
 * @property {EarRinging} ringing
 */

/**
 * @typedef {Object} EarCompareOptions
 * @property {number} [sampleRate]    rate of bare Float32Array clips
 * @property {boolean} [align=true]   align the onsets before comparing
 * @property {number} [maxShift=0.05] seconds of refinement around the onset alignment
 * @property {{envelope?: number, spectrum?: number, tonality?: number}} [weights]
 *                                    component weights of `score`
 *                                    (default 0.35 / 0.45 / 0.20)
 */

/**
 * compare() result. Both clips are brought to the lower of their two sample
 * rates (broaudio's polyphase resampler), each normalised to -23 on one
 * loudness scale for both (Measurement.loudness's blend weighted by the
 * *shorter* clip's duration: lufsShort when either clip is under 400 ms, lufs
 * when both are 800 ms or longer; -23 dBFS RMS when either has none), so
 * level never counts, and a short click and the same click with silence
 * after it are equally loud, and
 * onset-aligned (first envelope frame within 30 dB of its peak, then the
 * lag within ±maxShift with the smallest envelope distance). Lengths may
 * differ: the shorter clip counts as silence past its end, so ringing
 * longer or shorter than the reference is a real difference. Components are
 * dimensionless: ~0 is a match, ~1 is "very different" (they may exceed 1).
 * @typedef {Object} EarComparison
 * @property {number} score           weights.envelope·envelope + weights.spectrum·spectrum
 *                                    + weights.tonality·tonality (0.35 / 0.45 / 0.20);
 *                                    0 for a clip against itself
 * @property {number} envelope        envelopeDb / 30
 * @property {number} spectrum        (spectrogramDb + ltasDb) / 2 / 20
 * @property {number} tonality        0.5·|tonalityDiff| + 0.3·min(1, |log2 ringTimeRatio| / 2)
 *                                    + 0.2·|inharmonicityDiff|
 * @property {number} sampleRate      the rate the comparison ran at
 * @property {number} offsetTime      how much later the clip starts than the reference (s)
 * @property {number} loudnessDiffDb  clip loudness - reference loudness on that scale, before
 *                                    normalisation
 * @property {'lufs'|'lufsShort'|'blend'|'rms'} loudnessScale  the scale loudnessDiffDb and the
 *                                    normalisation used
 * @property {number} envelopeDb      mean |difference| of the two peak-relative envelopes
 *                                    (floored at -60 dB) over frames where either is above the floor
 * @property {number} spectrogramDb   mean |difference| of 40-band mel spectrograms (40 Hz ..
 *                                    min(16 kHz, Nyquist), dB floored 80 dB under the louder
 *                                    clip), over frames where either is within 60 dB of its peak
 * @property {number} ltasDb          mean |difference| of the 40-band long-term spectra, each
 *                                    normalised to its own total power (spectral balance alone)
 * @property {number} centroidRatio   clip centroid / reference centroid (>1 = brighter)
 * @property {number} tonalityDiff    clip tonality - reference tonality (>0 = more tonal)
 * @property {number} ringTimeRatio   (clip ringing.weightedRingTime + 0.05) / (reference's + 0.05)
 * @property {number} inharmonicityDiff
 * @property {number} durationDiff    clip tail end - reference tail end, after alignment (s)
 */

/**
 * @typedef {Object} EarSpectrogramOptions
 * @property {number} [sampleRate]         rate of bare Float32Array clips
 * @property {number} [width=800]          plot width of each panel, px
 * @property {number} [height=256]         plot height of each panel, px
 * @property {'stack'|'side'} [layout='stack']  panels one above another (times line up
 *                                         vertically) or side by side
 * @property {'log'|'mel'|'linear'} [scale='log']  frequency axis
 * @property {number} [minHz=30]           bottom of a log / mel axis (linear starts at 0)
 * @property {number} [maxHz]              top of the axis; default the lowest Nyquist of the clips
 * @property {number} [dbRange=80]         colour span below the top of the scale
 * @property {number} [maxDb]              top of the colour scale, dBFS; default the loudest bin of
 *                                         all clips, so every panel shares one scale
 * @property {number} [fftSize]            default ~43 ms at the clip's rate (2048 at 44.1/48 kHz)
 * @property {number} [fontScale=2]        pixel size of the 5x7 label font (1..4)
 * @property {string[]} [labels]           a title per clip (default: a path's file name, else "clip N");
 *                                         each panel's title also shows its duration
 * @property {string} [path]               also write a PNG here (through bro.image.encodePngFile
 *                                         when present, else a built-in uncompressed writer),
 *                                         resolved like fs.* paths
 */

/**
 * spectrogram() result: ImageData-shaped (width, height, data), plus where
 * each panel's plot area is and the shared scales. Colours are magma
 * (black = dbRange under the top, pale yellow = top). The time axis spans
 * 0 .. duration (the longest clip) in every panel with seconds labels and a
 * trailing "s"; a shorter clip's panel is hatched grey after its end. The
 * frequency axis is labelled in Hz (100, 1k, 10k), the colour bar in dBFS.
 * @typedef {Object} EarSpectrogram
 * @property {number} width
 * @property {number} height
 * @property {Uint8ClampedArray} data   RGBA, row-major, top row first
 * @property {number} duration
 * @property {number} minHz
 * @property {number} maxHz
 * @property {number} minDb
 * @property {number} maxDb
 * @property {{label: string, x: number, y: number, width: number, height: number,
 *             duration: number, sampleRate: number}[]} panels
 * @property {string} [path]            present when a PNG was written
 */

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * Measure one clip.
 * @param {EarClip} clip
 * @param {EarMeasureOptions} [opts]
 * @returns {EarMeasurement}
 * @throws {TypeError} for a value that is not a clip, or a bare Float32Array without opts.sampleRate
 * @throws {Error} when a path does not decode
 *
 * @example
 *   const m = bro.ear.measure('sfx/bell.wav');
 *   console.log(m.lufs, m.tailTime, m.t60, m.tonality);
 *   for (const p of m.partials) console.log(p.freqHz.toFixed(1), p.ringTime.toFixed(2), p.ratio);
 */
bro.ear.measure = function(clip, opts) {};

/**
 * How far `clip` is from `reference`. Lower is closer; the components say
 * what differs (envelope = timing and decay shape, spectrum = timbre and
 * balance, tonality = how pure and how long-ringing the partials are) and
 * the detail fields say in which direction.
 * @param {EarClip} clip
 * @param {EarClip} reference
 * @param {EarCompareOptions} [opts]
 * @returns {EarComparison}
 *
 * @example
 *   const d = bro.ear.compare(variant, 'refs/real_knock.wav');
 *   if (d.ringTimeRatio > 1.5) console.log('rings too long');
 *   if (d.centroidRatio > 1.3) console.log('too bright');
 */
bro.ear.compare = function(clip, reference, opts) {};

/**
 * Draw one clip, or several on one shared time, frequency and dB scale.
 * @param {EarClip|EarClip[]} clipOrClips
 * @param {EarSpectrogramOptions} [opts]
 * @returns {EarSpectrogram}
 *
 * @example
 *   const img = bro.ear.spectrogram([variant, ref], { labels: ['variant', 'ref'],
 *                                                     path: 'out/compare.png' });
 *   // or draw it: ctx.putImageData(new ImageData(img.data, img.width, img.height), 0, 0);
 */
bro.ear.spectrogram = function(clipOrClips, opts) {};

// ── bro.ear.fit — search a SynthGraph's parameters against a target ─────────
//
// Every number in a SynthGraph description is a named parameter
// (docs/audio-synth-graph-api.js). fit() renders the graph at candidate
// values, scores each render against a target, and searches for the values
// with the lowest distance. The distance is a weighted sum of
//
//   reference   bro.ear.compare(render, reference).score        × weights.reference (1)
//   measures    per measure() field, a unitless distance        × its own weight (1)
//   bands       per band, |band level - db| / 10                 × its own weight (1)
//   scorer      your function's distance per clip               × weights.scorer (1)
//   clap        1 - the CLAP score of prompt `index`             × weights.clap (1)
//
// Give at least one. They compose by plain addition, so a reference plus a
// CLAP prompt means "sound like this recording, and like 'a wooden knock' to
// CLAP": weigh them so neither swamps the other (compare scores sit near
// 0.05 for a close match and 0.3+ for a different sound; a measure term is 1
// for "clearly off": an octave, twice or half the time, 10 dB, 0.25 of a 0..1
// fraction).
//
// SEARCH. CMA-ES (covariance matrix adaptation, the standard method for 1 to
// ~30 continuous parameters with rough, non-smooth objectives) in a unit
// cube: each parameter's range maps linearly or logarithmically onto 0..1,
// so frequencies and times move by ratios. When a run converges or stalls
// (under 5 % better over its last 20 + 50·n/population generations) it
// restarts from a random point with twice the population, while budget
// remains; the best candidate of all runs is the result. Every candidate of a
// generation renders and is scored in parallel on native threads (the
// renders are thread-safe and bit-identical to the live voice).
//
// DETERMINISM. For a given `seed` the result, its history and its clip are
// identical bit for bit whatever `threads` is, sync or async. maxSeconds stops
// at a generation boundary, so a timed run is a prefix of the untimed one.
//
// JITTER. Renders use jitter: false by default, so the objective is a plain
// function of the parameters. With jitter: true each candidate renders once
// per render seed (`seeds`) and scores the mean; the same seeds serve every
// candidate, so the objective is still deterministic, and the fit prefers
// values that sound right across the graph's shot-to-shot variation. Noise
// nodes draw from the seed either way (seeds: 1 renders seed 1).
//
// EXTERNAL SCORERS. broaudio cannot link brosoundml, so CLAP joins through
// the JS layer: `clap.model` is anything with score(clip, prompts, opts?)
// returning {scores} (bro.ear.loadClap's ClapModel is), and embedText when
// it has one (string prompts are embedded once, up front). A `scorer`
// function works the same way for anything else. Both run on the JS thread,
// once per generation, over the whole generation's clips (population × seeds
// clips, candidate-major) — in the async form too, where the search thread
// hands the batch over and waits. A ClapModel scores ~50 ms per clip on the
// GPU, so a CLAP term costs about that per evaluation; keep the budget modest,
// or fit against a reference / measures first and add CLAP for a final pass
// started from those values (params[i].start).
//
// ASYNC. With opts.onDone the call returns an AsyncHandle at once and runs
// the search on a native thread. onProgress / onDone and the scorer calls run
// on the calling thread when the host ticks (bro's frame pump calls
// broaudio's tickAsyncJobs) or inside handle.wait(). cancel() stops at the
// next generation; onDone still fires, with the best result so far and
// {cancelled: true}. Bad options throw from the call itself, as in the sync
// form. Do not wait() on a fit from inside its own scorer or onProgress.
//
//   const g = new SynthGraph(knockDescription);
//   const r = bro.ear.fit(g, {
//     params: ['body.cutoff', 'body.decay', { name: 'res.freq', min: 200, max: 900 }],
//     reference: 'refs/knock.wav',
//     measures: { attackTime: 0.002 },
//     maxEvaluations: 800,
//   });
//   console.log(r.distance, JSON.stringify(r.params));
//   const buf = g.render(r.render);                 // the same samples as r.clip

/**
 * One searched parameter, the long form. Every field but `name` is optional.
 * Defaults: scale 'auto' is 'log' when the declared value is > 0 and the
 * graph's own range for it is non-negative and reaches at least 10
 * (frequencies, times, ratios, q, drive), else 'linear'. A log range defaults
 * to declared/4 .. declared×4 (two octaves either way); a linear one to the
 * graph's whole range when that spans at most 10 (pulse width, damping,
 * feedback), else declared ± |declared| (0 .. 2× for a positive value, so a
 * gain keeps its sign), or -1 .. 1 around 0 — always inside the graph's
 * range. Give ranges for anything that matters: sweep endpoints and mix
 * weights are plain numbers to the graph and default to linear.
 * @typedef {Object} EarFitParam
 * @property {string} name        the parameter's name (SynthGraph.paramNames)
 * @property {number} [min]
 * @property {number} [max]
 * @property {'auto'|'log'|'linear'} [scale='auto']  'log' needs min > 0
 * @property {number} [start]     where the first run starts; default the declared
 *                                value, clamped into the range
 */

/**
 * A measure() field to hit. `name` (the key in `measures`) is one of
 *   Hz, |log2(measured / target)|:            centroidHz, f0Hz (ringing.f0Hz),
 *                                             partialHz (the strongest partial)
 *   seconds, |log2((m + 2 ms) / (t + 2 ms))|: attackTime, onsetTime, envelopePeakTime,
 *                                             peakTime, tailTime, t60, weightedRingTime,
 *                                             strongestRingTime
 *   dB/s, |log2((|m| + 1) / (|t| + 1))|:      decayRate
 *   dB, |m - t| / 10:                         peakDb, envelopePeakDb, rmsDb, lufs,
 *                                             lufsShort, loudness
 *   0..1, |m - t| / 0.25:                     flatness, tonality, inharmonicity,
 *                                             ringScore, sparsity
 * A value the render does not have (t60 of a sound that does not decay,
 * partialHz with no partial) costs 2. Level fields (peakDb, rmsDb, lufs,
 * lufsShort, loudness) read the render as it is: loudness is not normalised
 * as compare() normalises it. For a cue that may render shorter than 400 ms
 * (UI clicks, ticks), target `loudness` (or `lufsShort`), not `lufs`: a
 * sub-block render's lufs is its energy over its own length, so a fit to it
 * trades level against decay time and tail length.
 * @typedef {number|{value: number, weight?: number, scale?: number}} EarFitMeasure
 *   `scale` replaces the divisor (1 octave, 1 doubling, 10 dB, 0.25).
 */

/**
 * @typedef {Object} EarFitOptions
 * @property {Array<string|EarFitParam>|Object<string, true|EarFitParam>} [params]
 *   what to search (at most 64); default every parameter not in `fixed`
 * @property {Object<string, number>|string[]} [fixed]  held at these values (an array
 *   holds the named parameters at their declared values). A parameter neither
 *   searched nor fixed keeps its declared value.
 * @property {EarClip} [reference]           compare() every render against it
 * @property {EarCompareOptions} [compare]   {align, maxShift, weights} for that comparison
 * @property {Object<string, EarFitMeasure>} [measures]
 * @property {{minHz: number, maxHz: number, db: number, weight?: number}[]} [bands]
 *   a band's share of the render's power, dB (10·log10(band power / total) of the
 *   long-term spectrum): {minHz: 2000, maxHz: 8000, db: -12} asks for a sixteenth
 *   of the power between 2 and 8 kHz
 * @property {function({samples: Float32Array, sampleRate: number, channels: 1}[]): ArrayLike<number>} [scorer]
 *   one distance per clip, lower is closer; a non-finite one counts as 2
 * @property {{model: ClapModel, prompts: ClapPrompt[], index?: number, weight?: number,
 *             options?: ClapClipOptions}} [clap]
 *   distance 1 - scores[index] (default 0); include anti-prompts, since CLAP's
 *   scores are relative to the prompts given. `options` (pad, long) go to score()
 * @property {{reference?: number, scorer?: number, clap?: number}} [weights]  default 1 each
 * @property {number} [maxEvaluations=1000]  candidates to evaluate at most; the fit stops when
 *   the next generation would pass it
 * @property {number} [maxSeconds]           wall-clock budget, checked between generations
 * @property {number} [stopAt]               stop once the best distance is at or below this
 * @property {number} [population]           candidates per generation; default
 *   2·(4 + floor(3·ln n)), at least 10 (14 for 3 parameters, 20 for 8)
 * @property {number} [sigma=0.25]           the first step, as a fraction of each range
 * @property {number} [seed=1]               the search's random seed
 * @property {number} [threads]              render threads; default the core count (max 32)
 * @property {boolean} [restarts=true]       restart converged runs with a bigger population
 * @property {boolean} [jitter=false]        render with the graph's jitter
 * @property {number|number[]} [seeds=1]     render seeds: a count (1..n) or a list; a
 *   candidate's distance is the mean over them
 * @property {number} [sampleRate]           render rate; default the reference's, else 48000.
 *   Also the rate of a bare Float32Array reference
 * @property {number} [maxDuration]          render cap, s; default 2 × the reference + 0.25,
 *   else 10 (most voices end by themselves first)
 * @property {boolean} [compiled=true]       render through the compiled kernels (same samples)
 * @property {{length: number, crossfade?: number, start?: number, snap?: boolean,
 *             curve?: 'auto'|'power'|'linear', releaseFade?: number}} [loop]
 *   every candidate renders one period of this seamless loop (SynthGraph.render's
 *   `loop`, see audio-synth-graph-api.js LOOPS) instead of a one-shot; maxDuration
 *   is unused. With snap (the default) periodic rates move to whole cycles per
 *   period (including one fed by a mix / mul of numbers), so a searched
 *   frequency lands on the snapped grid. The search itself runs over the
 *   unsnapped values (the distance is flat between grid points); with jitter
 *   off the result reports the SNAPPED values the clip plays, in params,
 *   searched[].value and render.params, so they round-trip:
 *   graph.values(r.render) equals r.params, and graph.render(r.render) is r.clip.
 *   With jitter on, the reported values are the pre-jitter, pre-snap ones
 *   (each seed snaps its own jittered values)
 * @property {function(EarFitProgress): (boolean|void)} [onProgress]  after every generation;
 *   in the sync form, returning false cancels
 * @property {function(?EarFitResult, {cancelled: boolean, error?: string})} [onDone]
 *   run asynchronously (see ASYNC above); the call returns an AsyncHandle
 */

/**
 * @typedef {Object} EarFitProgress
 * @property {number} generation       generations done
 * @property {number} evaluations
 * @property {number} restarts
 * @property {number} best             best distance so far
 * @property {number} generationBest   best of this generation
 * @property {number} sigma            the step size, unit-range units
 * @property {number} seconds
 * @property {Object<string, number>} params  the best values so far
 */

/**
 * @typedef {Object} EarFitResult
 * @property {Object<string, number>} params  every value to override to reproduce `clip`:
 *   the searched parameters' best values and the fixed ones
 * @property {{name: string, value: number, min: number, max: number,
 *             scale: 'log'|'linear', start: number}[]} searched   ranges as resolved
 * @property {number} distance
 * @property {{reference: number|null, comparison?: EarComparison,
 *             measures: Object<string, {target: number, measured: number|null, distance: number}>,
 *             bands: {minHz: number, maxHz: number, target: number, measured: number,
 *                     distance: number}[],
 *             external: number|null}} terms
 *   the best candidate's terms, unweighted, from its render at the first seed;
 *   `external` is the scorer + clap part as summed (weighted), averaged over the seeds
 * @property {number} evaluations
 * @property {number} generations
 * @property {number} restarts
 * @property {number} population       of the first run
 * @property {number} seconds
 * @property {'evaluations'|'time'|'stopAt'|'converged'|'cancelled'|'scorer'} stop
 *   'converged': converged with no budget left for a restart (or restarts: false)
 * @property {{generation: number, evaluations: number, best: number,
 *             generationBest: number, sigma: number, seconds: number}[]} history
 * @property {{samples: Float32Array, sampleRate: number, channels: 1}} clip  the best render
 * @property {{params: Object<string, number>, seed: number, jitter: boolean,
 *             sampleRate: number, maxDuration: number, loop?: object}} render
 *   SynthGraph.render options that reproduce `clip` bit for bit (`loop` when the
 *   fit rendered loops)
 */

/**
 * Search `graph`'s parameters for the render closest to the target.
 * Synchronous unless opts.onDone is given.
 * @param {SynthGraph|Object|string} graph  an instance, or a description
 * @param {EarFitOptions} opts
 * @returns {EarFitResult|AsyncHandle}
 * @throws {TypeError} for a bad option, an unknown parameter or measurement, or no target;
 *   the message starts with "bro.ear.fit: " and the option's path
 * @throws {RangeError} for a number out of range (an inverted range, a log range reaching 0)
 * @throws whatever `scorer`, a CLAP model's score() or onProgress throws (sync form; the
 *   async form reports it as onDone's `error`)
 *
 * @example
 *   // Recover a pluck from a recording, then check it by ear and eye.
 *   const g = new SynthGraph(pluckDesc);
 *   const r = bro.ear.fit(g, { params: ['o.freq', 'o.ratio', 'ie.decay', 'ae.decay'],
 *                              reference: 'refs/pluck.wav', maxEvaluations: 1500, stopAt: 0.01 });
 *   bro.ear.spectrogram([r.clip, 'refs/pluck.wav'], { labels: ['fit', 'ref'], path: 'out/fit.png' });
 *
 * @example
 *   // Hit measurements, keep the pitch, average over the graph's jitter.
 *   const r = bro.ear.fit(g, { fixed: { 'o.freq': 220 }, jitter: true, seeds: 4,
 *                              measures: { centroidHz: 1800, t60: { value: 0.6, weight: 2 } } });
 *
 * @example
 *   // Let CLAP steer, asynchronously, with progress.
 *   const clap = bro.ear.loadClap();
 *   const h = bro.ear.fit(g, {
 *     params: ['body.cutoff', 'body.decay', 'crack.gain'],
 *     clap: { model: clap, prompts: ['a wooden door knock', 'a xylophone', 'static noise'],
 *             options: { pad: 'silence' } },
 *     measures: { tailTime: 0.25 },
 *     maxEvaluations: 300,
 *     onProgress: p => console.log(p.evaluations, p.best.toFixed(3)),
 *     onDone: (r, info) => console.log(info.cancelled ? 'cancelled' : 'done', r.distance, r.params),
 *   });
 */
bro.ear.fit = function(graph, opts) {};

// ── bro.ear.loadClap — CLAP prompt scorer (brosoundml; ML builds only) ───────
//
// laion/larger_clap_general (HTSAT-base audio tower + RoBERTa text tower,
// 512-d joint space), hand-written on brotensor in brosoundml and pinned
// against transformers' ClapModel to ~1e-6 on embeddings (brosoundml
// docs/clap.md). brosoundml mounts loadClap and ClapModel onto the same
// `bro.ear` object when the build has BRO_WITH_SOUNDML; test for it with
// `typeof bro.ear.loadClap === 'function'`. CUDA by default: ~50 ms per 10 s
// window on the GPU, several seconds on CPU.
//
// Weights: brosoundml/weights/clap (scripts/download-clap.sh, then
// scripts/convert-clap.py). With no `dir`, loadClap looks under the root
// bro.tts.setAssetRoot set, then ../brosoundml, ./brosoundml and the CWD.
//
// Anti-prompts are the point: score a cannon against "heavy cannon firing",
// "metal footstep" AND "xylophone", and a synthetic-sounding variant shows up
// as mass moving onto the anti-prompt.
//
//   const clap = bro.ear.loadClap();
//   const prompts = ['heavy cannon firing', 'metal footstep', 'a xylophone'];
//   const text = clap.embedText(prompts);          // cache across a loop
//   for (const v of variants) {
//       const r = clap.score(v, text);             // cached embeddings in, same order
//       if (r.scores[0] > best) { best = r.scores[0]; keep = v; }
//   }

/**
 * A CLAP clip. Close to EarClip but not identical: a path must be a 16-bit
 * PCM WAV (decode other formats with AudioContext.decodeAudioFile first); a
 * bare Float32Array is at `opts.sampleRate` (default 48000). Channels are
 * averaged and any rate is resampled to 48 kHz.
 * @typedef {string|Float32Array|AudioBuffer|{samples: Float32Array, sampleRate: number, channels?: number}} ClapClip
 */

/**
 * A prompt: a string, or a Float32Array(512) text embedding from embedText.
 * @typedef {string|Float32Array} ClapPrompt
 */

/**
 * @typedef {Object} ClapClipOptions
 * @property {number} [sampleRate=48000] -  Rate of a bare Float32Array clip.
 * @property {'mean'|'crop'} [long='mean'] -  Clips over 10 s: 'mean' averages
 *   the embeddings of evenly spaced 10 s windows covering head and tail
 *   (deterministic; transformers crops at a random offset instead); 'crop'
 *   embeds one window.
 * @property {number} [cropAt] -  Window start in seconds for long: 'crop'
 *   (default: centred).
 * @property {'silence'|'repeat'|'auto'} [pad='auto'] -  Clips under 10 s:
 *   how they are filled out to the 10 s window. 'repeat' is the reference
 *   front-end (transformers' "repeatpad", the checkpoint's own
 *   preprocessor_config): the clip is tiled as many whole times as fit, then
 *   zeros, so a 0.3 s gunshot becomes 33 shots in a row and CLAP hears a
 *   drum loop or a rhythm. 'silence' places the clip once at the start and
 *   fills the rest with zeros (transformers' "pad"), so a one-shot is scored
 *   as one event. 'auto', the default, is 'silence' for clips under 2 s
 *   (measured at 48 kHz after resampling) and 'repeat', as the reference
 *   does, from 2 s to 10 s. Before 'auto', every short clip was repeated.
 *   Pass 'repeat' to get transformers' embedding of a short clip exactly.
 *   Clips over 10 s are never padded, so the option does nothing for them.
 *   Scores under different pads are not comparable; keep one choice across
 *   a comparison.
 * @property {function(*, {cancelled: boolean, error?: string})} [onDone] -
 *   Run on a work thread and call back with the result; the call then
 *   returns an AsyncHandle (as the other bro.tts / bro.stt async calls do)
 *   instead of the result. Without it the call blocks. One call at a time
 *   per model; a second while one runs throws.
 */

/**
 * @typedef {Object} ClapScore
 * @property {Float32Array} scores -  softmax(similarities * logitScale): one
 *   0..1 score per prompt, in order, summing to 1 over the prompts given. It
 *   is relative: it says which prompt the clip is closest to among THESE
 *   prompts, so always include anti-prompts.
 * @property {Float32Array} similarities -  Raw cosine similarity per prompt
 *   (-1..1), comparable across calls with different prompt lists.
 * @property {Float32Array} logits -  similarities * logitScale.
 * @property {Float32Array} embedding -  The clip's unit-length audio
 *   embedding (512); pass to scoreEmbedding to rescore without re-running
 *   the audio tower.
 * @property {number} bestIndex -  Index of the highest score.
 * @property {string|null} best -  That prompt's text; null when it was given
 *   as a cached embedding.
 */

class ClapModel {
  /** @readonly @type {boolean} */ loaded;
  /** 'CUDA' or 'CPU' (as the other bro.tts / bro.stt models report it). @readonly @type {string} */ device;
  /** 48000. @readonly @type {number} */ sampleRate;
  /** 512. @readonly @type {number} */ embeddingSize;
  /** 10: the audio tower's window. @readonly @type {number} */ windowSeconds;
  /** exp(learned logit scale), about 38.7. @readonly @type {number} */ logitScale;

  /**
   * @param {ClapClip} clip
   * @param {ClapPrompt|Array<ClapPrompt>} prompts
   * @param {ClapClipOptions} [opts]
   * @returns {ClapScore}
   */
  score(clip, prompts, opts) {}

  /**
   * @param {ClapClip} clip
   * @param {ClapClipOptions} [opts]
   * @returns {Float32Array} unit-length, 512
   */
  embedAudio(clip, opts) {}

  /**
   * @param {string|Array<string>} prompts
   * @returns {Float32Array|Array<Float32Array>} one embedding, or one per prompt
   */
  embedText(prompts) {}

  /**
   * score() for an audio embedding already in hand.
   * @param {Float32Array} embedding
   * @param {ClapPrompt|Array<ClapPrompt>} prompts
   * @returns {ClapScore}
   */
  scoreEmbedding(embedding, prompts) {}

  /** Frees the weights; the model is unusable afterwards. */
  dispose() {}
}

/**
 * Loads CLAP. Blocking unless `opts.onReady` is given, in which case it loads
 * on a work thread, returns an AsyncHandle, and calls onReady(model) or
 * onError(message).
 * `new bro.ear.ClapModel()` throws; the class is exported for instanceof.
 * @param {string} [dir] -  A converted checkpoint directory (model.safetensors +
 *   tokenizer files); defaults as described above.
 * @param {{device?: 'cuda'|'cpu', onReady?: function(ClapModel), onError?: function(string)}} [opts]
 * @returns {ClapModel}
 */
bro.ear.loadClap = function(dir, opts) {};
