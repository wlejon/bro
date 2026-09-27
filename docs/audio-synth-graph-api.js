/**
 * =============================================================================
 * SynthGraph — procedural sound voices from a declarative graph
 * =============================================================================
 *
 * A synthesis graph describes a sound as a small graph of generators and
 * processors (oscillators, FM, noise, random impulses, filters, envelopes,
 * sweeps, shapers, resonator banks, combs, layers) in a plain object. One
 * description gives every trigger a fresh variation (seeded per-parameter
 * jitter), and the same graph serves three uses:
 *
 *   new SynthGraph(desc)                 parse + validate (or ctx.createSynthGraph)
 *   graph.render(opts?)                  offline: a mono AudioBuffer, at once,
 *                                        no device, no file (bro.ear takes it);
 *                                        with `loop`, one period of a seamless loop
 *   ctx.playSynth(graph, opts?)          live: a playback id, plain or positional;
 *                                        with `loop`, it loops until released
 *   ctx.releaseSynth(id)                 note-off (envelopes move to release)
 *
 * The graph is the SOURCE STAGE of the engine's per-voice chain: a playing
 * synth voice then goes through the same distance / air / propagation-delay /
 * HRTF / bus path as a clip playback. render() produces exactly (bit for bit)
 * the samples the voice produces in the engine before that chain, for the
 * same seed, params and sample rate — so a sound tuned offline (e.g. against
 * bro.ear.compare) plays back as tuned.
 *
 * SHAPE AND PARAMETERS. The wiring and the choices (wave, colour, mode,
 * curve) are the graph's *shape*; every number is a *parameter*. Each layer
 * shape is compiled once to native code (brass JIT) and shared by every voice
 * of that shape in every graph, so 100 voices of one gunshot with different
 * jitter cost one compile. Until a shape's kernel is ready (tens of ms, on a
 * background thread; `precompile()` does it now, blocking) voices run in the
 * interpreter, which produces the same samples. Builds without a JIT backend
 * always interpret.
 *
 * DETERMINISM. Output depends only on (description, seed, params, jitter
 * flag, sample rate): not on block sizes, threads, the compiled/interpreted
 * choice or the machine's FPU mode. render() is thread-safe and cheap to call
 * in a loop.
 *
 * `SynthGraph` is a global in the main realm and in every Worker realm: it
 * needs no engine, so a worker can construct and render() graphs (for
 * example to search parameters in parallel), while playing one needs the
 * main realm's AudioContext.
 *
 * @example
 *   // A gunshot: a highpassed noise crack plus a swept, driven body 2 ms later.
 *   const gunshot = new SynthGraph({
 *     layers: {
 *       crack: { nodes: {
 *         n:  { type: 'noise', color: 'white', gain: 'e' },
 *         e:  { type: 'env', attack: 0.0005, decay: { value: 0.03, jitter: 0.2 }, sustain: 0, release: 0.01 },
 *         hp: { type: 'filter', mode: 'highpass', input: 'n', cutoff: { value: 1800, jitter: 0.1 }, q: 0.7 },
 *       }, output: 'hp' },
 *       body: { offset: 0.002, gain: 0.8, nodes: {
 *         bn: { type: 'noise', color: 'pink' },
 *         sw: { type: 'sweep', from: 1400, to: 180, time: 0.12, curve: 'exp' },
 *         bp: { type: 'filter', mode: 'bandpass', input: 'bn', cutoff: 'sw', q: 1.5, gain: 'be' },
 *         be: { type: 'env', attack: 0.001, decay: 0.25, sustain: 0, release: 0.05 },
 *         sh: { type: 'shaper', mode: 'tanh', input: 'bp', drive: 2.5 },
 *       }, output: 'sh' },
 *     },
 *   });
 *   gunshot.precompile();                                   // optional
 *   const ctx = new AudioContext();
 *   ctx.playSynth(gunshot, { position: [12, 0, -30] });     // every shot differs
 *
 * @example
 *   // Tune a parameter offline against a reference recording.
 *   const bar = new SynthGraph(metalBarDesc);
 *   let best = null;
 *   for (let f = 400; f <= 700; f += 10) {
 *     const clip = bar.render({ seed: 1, params: { 'res.freq': f } });
 *     const d = bro.ear.compare(clip, 'refs/bar.wav').score;
 *     if (!best || d < best.d) best = { f, d };
 *   }
 *   ctx.playSynth(bar, { params: { 'res.freq': best.f } });
 *
 * @example
 *   // A tank track: links clanking at a jittered rate (impulses exciting a
 *   // resonator) over an engine drone, as a seamless 2 s loop that plays
 *   // until the tank stops, then rings out through its release.
 *   const track = new SynthGraph({ layers: {
 *     links: { nodes: {
 *       k:   { type: 'impulses', shape: 'decay', rate: 9, jitter: 0.3, length: 0.004, ampJitter: 0.5 },
 *       res: { type: 'resonator', input: 'k', freq: 900, modes: [
 *                { ratio: 1, decay: 0.08, gain: 0.4 }, { ratio: 2.7, decay: 0.05, gain: 0.2 }] },
 *     }, output: 'res' },
 *     drone: { gain: 0.4, nodes: {
 *       s:  { type: 'osc', wave: 'saw', freq: 47, gain: 'e' },
 *       e:  { type: 'env', attack: 0.3, decay: 0.1, sustain: 0.8, release: 0.6 },
 *       lp: { type: 'filter', input: 's', cutoff: 400, q: 2 },
 *     }, output: 'lp' },
 *   } });
 *   const id = ctx.playSynth(track, { loop: { length: 2, start: 0.5 }, position: [0, 0, -20] });
 *   // ... later
 *   ctx.releaseSynth(id);
 */

/* =============================================================================
 * THE DESCRIPTION
 * =============================================================================
 *
 * A graph is either one layer:
 *
 *   { nodes: { <id>: <node>, ... }, output: '<id>', duration?: <number> }
 *
 * or several layers summed, each with a start offset and a gain:
 *
 *   { layers: { <layerId>: { nodes: {...}, output: '<id>',
 *                            offset?: <seconds, 0>, gain?: <1> }, ... },
 *     duration?: <seconds> }
 *
 * Up to 16 layers, 48 nodes per layer. Ids (node and layer) are letters,
 * digits and _, not starting with a digit, and unique across the whole graph.
 * Inputs connect only within a layer. The graph must be acyclic (feedback
 * lives inside `fm` and `comb`); every node must reach its layer's output.
 * Unknown fields are errors.
 *
 * `duration` cuts the voice at exactly that many seconds (a drone with no
 * envelope needs it to end at all).
 *
 * NUMBERS. Every numeric field is a parameter, written as a number or as
 *
 *   { value, jitter?, jitterAbs? }
 *
 * On each trigger: v' = value * (1 + jitter * u1) + jitterAbs * u2, with
 * u1, u2 uniform in [-1, 1) from the trigger's seed (independent per
 * parameter), then clamped to the field's range. jitter is 0..1.
 *
 * SIGNAL INPUTS. Fields marked (signal) below take either a number/parameter
 * (a constant) or a node id string (that node's output, per sample): a
 * filter's cutoff can be a `sweep`, an oscillator's freq an `osc` + `mix`
 * vibrato, a gain an `env`. `input` fields require a node id.
 *
 * Every node has `gain` (signal, default 1): its output is multiplied by it.
 *
 * PARAMETER NAMES are the number's path without the containers:
 *   '<nodeId>.<field>'                  'hp.cutoff', 'e.decay'
 *   '<nodeId>.modes.<i>.<ratio|decay|gain>'
 *   '<nodeId>.inputs.<i>' / '.inputs.<i>.weight'   (mix)
 *   '<nodeId>.segments.<i>.<time|level>'           (segment env)
 *   '<layerId>.offset', '<layerId>.gain', 'duration'
 * A field left out still exists as a parameter at its default, so it can be
 * overridden per trigger. graph.paramNames lists them all.
 *
 * NODES
 *
 *   osc        { wave: 'sine'|'saw'|'square'|'triangle', freq (signal, Hz, 440),
 *                pm (signal, phase offset in cycles, 0), pw (signal, square
 *                pulse width 0..1, 0.5; clamped to 0.02..0.98) }
 *              Output -1..1. Saw and square are band-limited (PolyBLEP).
 *
 *   fm         { freq (signal, carrier Hz, 440), ratio (signal, modulator =
 *                freq * ratio, 1), index (signal, modulation index in
 *                radians, 1), feedback (modulator self-feedback 0..10, 0) }
 *              Two sine operators. Output -1..1.
 *
 *   noise      { color: 'white'|'pink'|'brown' }
 *              Seeded per trigger. White -1..1; pink (Kellet) and brown
 *              (leaky integrator) scaled to about the same level.
 *
 *   filter     { mode: 'lowpass'|'highpass'|'bandpass'|'notch', input,
 *                cutoff (signal, Hz, 1000), q (signal, 0.05..1000, 0.707) }
 *              Topology-preserving state-variable filter, 12 dB/oct; stable
 *              under per-sample cutoff modulation. Bandpass has unity gain
 *              at the centre.
 *
 *   env        ADSR form: { attack (s, 0.005), decay (s, 0.1), sustain
 *                (level, 1), release (s, 0.1), peak (level, 1), gate? (s) }
 *              Linear attack from 0 to peak; decay and release are 'decay'
 *              curves. Holds at sustain until releaseSynth / the gate; a
 *              sustain of 0 with no gate ends the envelope after its decay
 *              (a percussive envelope).
 *
 *              Segment form: { start (level, 0), segments: [{ time (s),
 *                level, curve?: 'linear'|'exp'|'decay' }, ...] (1..16),
 *                hold? (segment index), gate? (s) }
 *              Moves from `start` through each segment's level over its
 *              time. With `hold: i` it holds after segment i until release,
 *              then continues with segment i+1.
 *
 *              `gate` releases the envelope that many seconds after the
 *              layer starts (a note length inside the description).
 *
 *              Curves: 'linear'; 'exp' (geometric: constant ratio per
 *              sample; falls back to linear when either end is 0 or they
 *              differ in sign); 'decay' (one-pole approach covering 60 dB of
 *              the distance over the segment's time, then landing exactly on
 *              the level: the natural shape for decays and releases).
 *
 *              Every `env` node (whatever it modulates) counts toward the
 *              voice's end, see VOICE END.
 *
 *   sweep      { from, to, time (s), delay? (s, 0), curve? ('exp') }
 *              Holds `from` for `delay`, moves to `to` over `time`, then
 *              holds `to`. For pitch and filter sweeps (feed it to freq /
 *              cutoff). Does not count toward the voice's end.
 *
 *   shaper     { mode: 'tanh'|'clip'|'fold', input, drive (signal, 1) }
 *              Waveshaper on input * drive: a rational tanh (saturates at
 *              +-1 by |x| = 3), a hard clip at +-1, or a triangle wavefolder.
 *
 *   resonator  { input, freq (Hz, 440), modes: [{ ratio (1), decay (s to
 *                -60 dB, 1), gain (1) }, ...] (1..24) }
 *              A bank of two-pole resonators at freq * ratio, each ringing
 *              for its decay time; an impulse makes mode m ring at amplitude
 *              gain_m. Metallic and wooden bodies: excite with a short noise
 *              burst. Modes at or above 0.49 * sampleRate are silent.
 *
 *   comb       { input, freq (Hz of the delay, 220), feedback (-0.9999..0.9999,
 *                0.7), damp (0..1, 0) }
 *              Feedback comb, delay 1/freq with fractional interpolation and
 *              a one-pole lowpass in the loop (damp 0 = none): plucked-string
 *              and tube resonances.
 *
 *   impulses   { shape: 'impulse'|'rect'|'hann'|'decay', rate (signal, events
 *                per second, 0..24000, 10), jitter (0..1, 0), length (grain
 *                seconds, 0.0001..10, 0.005), ampJitter (0..1, 0) }
 *              Stochastic events at a mean `rate`: rattles, clanks, track
 *              links, crackle, rain. An event phase advances by rate / sampleRate
 *              each sample; an event fires when it reaches a threshold that
 *              every event redraws as 1 + jitter * u (u uniform in [-1, 1),
 *              floored at 0.05), so jitter 0 is periodic, jitter 1 spreads the
 *              intervals over 0.05..2x the mean, and the mean rate holds
 *              either way. Each event draws its amplitude 1 - ampJitter * u'
 *              (u' in [0, 1)) and starts a grain:
 *                'impulse'  one sample at the amplitude (length unused): the
 *                           exciter for a resonator or comb
 *                'rect'     the amplitude for `length`, then 0: a gate
 *                'hann'     a sin^2 window over `length`: smooth grains, a
 *                           gate for noise (mul)
 *                'decay'    the amplitude falling 60 dB over `length`
 *                           (exponential, it keeps falling): clicks
 *              A new event restarts the grain. The first event fires on the
 *              layer's first sample (offset the layer to delay it); rate 0
 *              gives that one event only. A wired rate (a sweep) slows or
 *              speeds the events per sample; at most one event every other
 *              sample. The events draw from the trigger's seed (their own
 *              stream, independent of noise nodes), so a seed gives the same
 *              events every time, compiled or interpreted. Output 0..1 times
 *              gain. Its parameters fit like any other: 'k.rate', 'k.jitter',
 *              'k.length', 'k.ampJitter'.
 *
 *   mul        { a, b }            a * b (ring modulation, VCA).
 *
 *   mix        { inputs: [ '<id>' | number | {node: '<id>', weight} , ... ] (1..16) }
 *              Weighted sum. A number input is a constant (useful as an
 *              offset: [220, {node: 'lfo', weight: 6}] is 220 Hz +-6).
 *
 * VOICE END. A voice finishes when every `env` node has finished and the
 * output then stays below -100 dBFS over a 256-sample window, or at
 * `duration`. A graph without `env` nodes and without `duration` never ends
 * by itself (render() cuts it at maxDuration; a playback plays until
 * stopClip). A looping voice ends only after releaseSynth (or stopClip).
 *
 * LOOPS. `loop: { length, crossfade?, start?, snap?, curve?, releaseFade? }`
 * on render() or playSynth() cuts a seamless loop from the voice:
 *
 *   length       seconds of one period (0.001..60, required)
 *   crossfade    seconds (0..30, 0.05; clamped to length / 2)
 *   start        seconds of the voice before the loop (0..600, 0.25): the
 *                warm-up, past the attack and long enough for filters,
 *                resonators and combs to settle
 *   snap         true (default): put periodic rates on whole cycles
 *   curve        'auto' (default) | 'power' | 'linear': the crossfade's gains
 *   releaseFade  seconds (0..10, 0.02): a looping voice's note-off crossfade
 *
 * The voice renders from `start` for length + crossfade seconds, with no
 * end (`duration`, finished envelopes and the silence floor are ignored).
 * Loop sample i is voice sample start + i, except over the first `crossfade`
 * of the loop, where the voice's continuation past the loop's end fades out
 * while the loop's own start fades in. So the SEAM IS THE VOICE ITSELF: the
 * loop's last sample and its first are two consecutive samples the voice
 * produced, and every filter, resonator and comb state is carried across the
 * wrap by construction (no wrapped filtering, no click); the blend sits inside
 * the loop, between two stretches of the same sound. 'power' is equal power
 * (right for noise), 'linear' equal gain (right for identical halves, a
 * snapped settled tone), and 'auto' measures the two halves' correlation and
 * corrects equal power for it (1: equal gain, 0: equal power). crossfade 0
 * is a hard cut (the voice's frames exactly; the seam is not continuous).
 *
 * snap moves every constant periodic rate to a whole number of cycles per
 * period, at least one: oscillator frequencies, FM carrier frequencies (the
 * ratio follows so the modulator is whole too) and impulse rates. A 0.3 Hz
 * LFO in a 0.5 s loop becomes 2 Hz; a 55.3 Hz saw becomes 56 Hz. Frequencies
 * that are signals (a sweep, a mix) are not snapped. A snapped settled tone
 * tiles as one continuous waveform.
 *
 * render({ loop }) returns exactly one period (length * sampleRate samples,
 * rounded); maxDuration does not apply. playSynth(graph, { loop }) renders
 * the loop, and its release tail, when called (on the calling thread; a
 * 2 s loop is milliseconds of work) and then plays that period over and
 * over: bit for bit what render({ loop }) returns for the same seed /
 * params / jitter / rate, before the distance chain. Its release: the loop
 * crossfades (equal power, releaseFade) into the voice's own release, which
 * is the voice continued from the loop's seam with its envelopes released
 * (capped at 10 s, faded if still sounding); a graph without an `env` node
 * has no release and fades out over releaseFade.
 *
 * bro.ear.fit takes the same `loop` option, so a loop's parameters can be
 * searched as loops.
 *
 * ERRORS. A description that does not validate throws a TypeError, or a
 * RangeError for a well-formed number outside its range. The message is
 * 'SynthGraph: ' + the path of the offending field + what is wrong:
 *   'SynthGraph: layers.body.nodes.sw.curve: must be 'linear', 'exp' or 'decay''
 *   'SynthGraph: nodes.o.freq: -5 is out of range [0, 1e+06]'
 *   'SynthGraph: nodes: cycle: a -> b -> a (feedback is not supported; ...)'
 */

class SynthGraph {
  /**
   * Parse and validate a description.
   * @param {object|string} description  a plain object (see above) or its JSON text
   * @throws {TypeError|RangeError}
   */
  constructor(description) {}

  /**
   * Every parameter by name: its declared value, its jitter and the range a
   * value is clamped into.
   * @type {Object<string, {value: number, jitter: number, jitterAbs: number, min: number, max: number}>}
   */
  get params() {}

  /** @type {string[]} every parameter name, in declaration order */
  get paramNames() {}

  /**
   * The layers, in declaration order. `shape` is the canonical key the
   * compiled kernel is cached under: two layers (in any graphs) with equal
   * shapes share one kernel. A single-layer graph's layer id is 'main'.
   * @type {{id: string, shape: string}[]}
   */
  get layers() {}

  /** @type {boolean} whether every layer's compiled kernel is ready */
  get compiled() {}

  /**
   * Compile every layer's kernel now (blocking; tens of ms per shape the
   * process has not compiled before, nothing for the rest).
   * @returns {boolean} true when all are ready; false without a JIT backend
   */
  precompile() {}

  /**
   * Render one trigger offline, synchronously, to a mono AudioBuffer (which
   * bro.ear.measure / compare / spectrogram accept). Bit-identical to the
   * voice ctx.playSynth plays for the same seed / params / jitter at the same
   * rate, before its distance chain.
   *
   * @param {object} [opts]
   * @param {number}  [opts.sampleRate]   8000..384000; default the audio
   *                                      engine's rate when one exists, else 48000
   * @param {number}  [opts.seed=0]       uint32: selects the jitter and the noise
   * @param {Object<string, number>} [opts.params]  overrides of declared values
   *                                      by parameter name (applied before jitter)
   * @param {boolean} [opts.jitter=true]  false renders the declared values exactly
   * @param {number}  [opts.maxDuration=10]  seconds (0..600): cut a voice that
   *                                      has not ended by then
   * @param {boolean} [opts.compiled=true]  use (and first compile, blocking) the
   *                                      kernels; false interprets. Same samples.
   * @param {object}  [opts.loop]         {length, crossfade, start, snap, curve,
   *                                      releaseFade}: render one period of a
   *                                      seamless loop (LOOPS above)
   * @returns {AudioBuffer}  length = where the voice ended (a loop: one period)
   * @throws {TypeError|RangeError}  unknown option or parameter, bad value
   */
  render(opts) {}
}

/**
 * AudioContext methods.
 */
class AudioContext {
  /**
   * Same as `new SynthGraph(description)`.
   * @param {object|string} description
   * @returns {SynthGraph}
   */
  createSynthGraph(description) {}

  /**
   * Start a synth voice now or at `when`. The id is a playback id: the
   * setPlayback* methods for gain, pan, bus and spatial position / velocity,
   * and stopClip / isClipPlaying, apply to it as to a clip playback (those
   * about clip position, loop and rate do not). The voice is removed when it
   * finishes (VOICE END).
   *
   * @param {SynthGraph|object|string} graph  a SynthGraph, or a description
   *                                      (parsed on each call: prefer a SynthGraph)
   * @param {object} [opts]
   * @param {number}  [opts.seed]         uint32; default a new seed per call (per
   *                                      graph), so repeated triggers vary
   * @param {Object<string, number>} [opts.params]  overrides by parameter name
   * @param {boolean} [opts.jitter=true]
   * @param {number}  [opts.gain=1]       0..1000
   * @param {number}  [opts.when]         context time (s) to start; default now
   * @param {number}  [opts.pan=0]        -1..1 (non-positional voices)
   * @param {number}  [opts.bus=0]        bus id to route to
   * @param {number[]} [opts.position]    [x, y, z]: makes the voice positional
   *                                      (distance, air, delay, HRTF as clips)
   * @param {object}  [opts.loop]         {length, crossfade, start, snap, curve,
   *                                      releaseFade}: a looping voice, playing
   *                                      render({loop})'s period until releaseSynth
   *                                      (LOOPS above); rendered here, on this thread
   * @returns {number}  playback id
   * @throws {TypeError|RangeError}
   */
  playSynth(graph, opts) {}

  /**
   * Note-off: every held envelope of the voice (an ADSR's sustain, a segment
   * envelope's `hold`) moves on to its release. A looping voice crossfades
   * into its release tail and then ends. Unknown ids are ignored.
   * @param {number} id  from playSynth
   */
  releaseSynth(id) {}
}

/*
 * COST (source stage only, per voice per 128-frame block at 48 kHz, one
 * machine; 100 voices of one graph with distinct seeds):
 *
 *   graph                         interpreted   compiled   100 voices, compiled
 *   gunshot (noise + swept body)     6.9 us      3.7 us     14% of real time
 *   FM bell (2-op, index env)        3.6 us      2.8 us     11%
 *   metal hit (6-mode resonator)     2.6 us      0.9 us      3%
 *
 * A per-sample (wired) filter cutoff or oscillator frequency costs more than
 * a constant one: the coefficients are recomputed every sample.
 */
