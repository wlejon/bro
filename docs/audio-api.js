/**
 * =============================================================================
 * AudioContext & friends — Web-Audio-shaped front end over broaudio
 * =============================================================================
 *
 * The binding lives in broaudio (`../broaudio/src/api`) and is installed once
 * per realm on the main thread. It is NOT available inside a Worker.
 *
 * broaudio is not a node-graph renderer. It is a synth-voice + clip-player
 * engine with a fixed set of mix buses, each carrying a fixed effect chain
 * (4 filter slots, delay, compressor, chorus, reverb, 7-band EQ, distortion),
 * a master limiter and a 3D spatializer. The Web Audio classes are a thin
 * translation layer onto that engine, so read these rules before relying on
 * Web Audio semantics:
 *
 * - ONE ENGINE. Every AudioContext drives the same process-wide engine.
 *   `currentTime` never restarts, `suspend()` on one context pauses all of
 *   them, and constructing a context un-pauses the engine.
 * - SOURCES PLAY WITHOUT BEING CONNECTED. An OscillatorNode is an engine
 *   voice and an AudioBufferSourceNode is a clip playback; both go straight
 *   to the master bus on `start()`, connected to `destination` or not.
 * - THE GRAPH'S SHAPE IS READ AT start(); ITS PARAMS STAY LIVE. `start()`
 *   walks the nodes downstream of the source once and records which
 *   GainNodes, StereoPannerNode and PannerNode lie on its path. Connecting or
 *   disconnecting nodes afterwards does not re-route a playing source. The
 *   params of the nodes it found do stay live: the source's gain is its own
 *   gain times every GainNode's gain on the path, its pan follows the
 *   StereoPannerNode, its 3D position follows the PannerNode's
 *   positionX/Y/Z, and its pitch follows its frequency (or playbackRate) and
 *   detune. A `.value` write or a scheduling call on any of those params
 *   reaches a source that is already playing. The PannerNode's distance
 *   settings (refDistance, maxDistance, rolloffFactor, distanceModel) are
 *   copied at `start()` only.
 * - EFFECT NODES ARE MASTER-BUS EFFECTS. BiquadFilterNode owns one of the
 *   master bus's 4 filter slots, which is switched on only while the filter
 *   node itself is connected (`filter.connect(...)`). Connecting anything to
 *   or from a DelayNode, DynamicsCompressorNode, WaveShaperNode or
 *   ConvolverNode switches on the master bus's delay, compressor, distortion
 *   (soft clip) or algorithmic reverb at 100% wet, and `disconnect()`
 *   switches it off again. They filter the whole mix, not only their input.
 *   For an AudioBufferSourceNode the same nodes are ALSO applied offline to
 *   the buffer at `start()` (biquad, delay shift, the real WaveShaper curve,
 *   the real Convolver impulse, compressor), so a buffer source routed
 *   through them is processed twice.
 * - AUTOMATION RUNS ON THE CONTROL TICK, NOT PER SAMPLE. AudioParam keeps a
 *   Web Audio timeline (set / linear / exponential / target / curve). The
 *   binding evaluates it at `currentTime` and pushes the result to the
 *   engine when a param is set or scheduled, on every host frame tick, and
 *   on each 128-frame quantum of `ctx.renderBlock()`. Between those points
 *   the engine holds the last value, so a ramp moves in steps at frame rate
 *   (in headless rendering, at the 128-frame quantum). A param reaches the
 *   engine only through something that uses it: a playing source whose path
 *   it lies on (see above), a BiquadFilterNode's slot, an oscillator's own
 *   envelope / pitch-bend params, or a DelayNode / DynamicsCompressorNode
 *   bound to the master bus (see those classes). Reading `.value` evaluates
 *   the timeline but pushes nothing. Automation never overwrites `.value`'s
 *   stored base, which is what a first ramp starts from.
 * - EVENTS ARRIVE ON THE FRAME TICK. The only event is
 *   AudioBufferSourceNode `onended`. It, and the automation above, are
 *   delivered from the host's once-per-frame tick (the binding's
 *   `tickAsyncJobs()`, the same tick that settles `createClipFromFileAsync`),
 *   and at the end of each `ctx.renderBlock()`. There is no `onstatechange`,
 *   AudioWorklet, ScriptProcessorNode, OfflineAudioContext,
 *   MediaElementSource, ConstantSourceNode or IIRFilterNode.
 * - GARBAGE COLLECTION STOPS SOUND, EXCEPT A PLAYING BUFFER SOURCE. A
 *   collected OscillatorNode stops and frees its voice, and a collected
 *   BiquadFilterNode releases its slot. Keep a reference to anything that
 *   should keep playing. A started AudioBufferSourceNode that is playing a
 *   buffer is held by the binding, as Web Audio holds it, until the tick
 *   that sees it end and fires `onended`. After that, collecting it stops
 *   its playback and deletes its clip.
 * - UNDERSCORE PROPERTIES ARE INTERNAL. The binding keeps graph edges and
 *   callbacks as plain properties on the JS objects so the collector sees
 *   them: `_targets` (connect() destinations), `_buffer`, `_ch<n>`
 *   (getChannelData views), `_pan`, and on the synth helpers `_laneCbs`,
 *   `_voiceSetup`, `_cc_<n>`, `_rawCb`, `_pitchBendCb`,
 *   `_connectedAllocator` and `_allocator`. They are not API: do not read,
 *   write or rely on them.
 * - Node constructors (`new GainNode()`, `new DelayNode(maxTime)`, ...) ignore
 *   a context argument and option dictionaries; the `ctx.create*` factories
 *   are the normal path. `new PeriodicWave(ctx, {real, imag})` and
 *   `new AudioBuffer({...})` are the exceptions that read their options.
 *   `AudioNode`, `AudioParam` and `AudioDestinationNode` exist for
 *   `instanceof` and throw TypeError when constructed.
 * - TYPED ARRAYS ARE CHECKED. A parameter documented as Float32Array (or
 *   Uint8Array) throws TypeError for any other element type, and for a view
 *   whose buffer has been detached (transferred). It never reinterprets
 *   another array's bytes. Where a plain array of numbers is also accepted,
 *   the parameter says so.
 *
 * Next to the Web Audio subset, AudioContext carries broaudio's engine API
 * directly, addressed by integer ids: voices (`createVoice`), clips
 * (`createClip*`, `playClip`), playbacks, streams, buses and their effects,
 * wavetables, presets and offline rendering, plus the synth helpers
 * VoiceAllocator, ModMatrix, MidiInput and Sequence. Anything the Web Audio
 * layer cannot express (live gain changes, moving a sound in 3D, per-bus
 * effects) is done there. That half is documented in `audio-engine-api.js`.
 *
 * File paths (`decodeAudioFile`, `saveWav`, `exportRecordingToWav`) resolve
 * the way `fs.*` does: relative to the app directory, mount paths honoured.
 * A path being written resolves through its parent directory, which must
 * exist.
 *
 * Live microphone chunks for speech/ML consumers are `bro.mic`, documented in
 * `mic-api.js`; see also the "Microphone" section at the end of this file.
 *
 * @example
 *   // Web Audio style: a synth voice through a gain and a stereo panner
 *   const ctx = new AudioContext();
 *   const osc = ctx.createOscillator();
 *   const g = ctx.createGain();
 *   const pan = ctx.createStereoPanner();
 *   osc.type = 'sawtooth';
 *   osc.frequency.value = 220;
 *   g.gain.value = 0.3;
 *   pan.pan.value = -0.5;
 *   osc.connect(g).connect(pan).connect(ctx.destination);
 *   osc.start();
 *   osc.stop(ctx.currentTime + 1);
 *   // Live: the voice plays at osc.gain * g.gain, so this fades it to 0.03
 *   // over half a second, stepped on the frame tick.
 *   g.gain.linearRampToValueAtTime(0.1, ctx.currentTime + 0.5);
 *
 * @example
 *   // A buffer source with a loop window and an ended handler
 *   const src = ctx.createBufferSource();
 *   src.buffer = buf;                          // plays at buf.sampleRate
 *   src.loop = true;
 *   src.loopStart = 0.5;
 *   src.loopEnd = 1.5;                         // seconds into the buffer
 *   src.onended = (e) => console.log('done', e.target === src);
 *   src.connect(ctx.destination);
 *   src.start(0, 0.25, 4);                     // from 0.25 s, 4 s of content
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * The fields `decodeAudioFile` returns, and that `decodeAudioData` also sets
 * on the AudioBuffer it resolves with (and on the returned promise itself).
 * Audio is always resampled to the engine rate.
 * @typedef {Object} AudioDecodedBuffer
 * @property {Float32Array} samples -  Interleaved samples, `numFrames * channels` long.
 * @property {number} channels -  Channel count of the file.
 * @property {number} sampleRate -  The ENGINE sample rate (not the file's).
 * @property {number} numFrames -  Frames after resampling.
 */

/**
 * The third argument of `createPeriodicWave` / `new PeriodicWave(real, imag,
 * options)`. `new PeriodicWave(ctx, options)` takes `real` and `imag` in the
 * same object.
 * @typedef {Object} PeriodicWaveOptions
 * @property {Float32Array|Array<number>} [real] -  `new PeriodicWave(ctx, options)` only.
 * @property {Float32Array|Array<number>} [imag] -  `new PeriodicWave(ctx, options)` only.
 * @property {boolean} [disableNormalization=false] -  Keep the summed table's
 *   amplitude instead of scaling its peak to 1.
 */

/**
 * The option object of `new AudioBuffer(options)`.
 * @typedef {Object} AudioBufferOptions
 * @property {number} length -  Frames; must be positive (TypeError otherwise).
 * @property {number} [numberOfChannels=1] -  Clamped to 1..32.
 * @property {number} [sampleRate] -  Default: the context (engine) rate. The
 *   rate the frames are at; see AudioBuffer.
 */

/**
 * `getUserMedia` constraints. Ignored: the call always opens the default
 * capture device.
 * @typedef {Object} MediaStreamConstraints
 * @property {boolean} [audio]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * An automatable value. Every node param is one of these.
 *
 * Differences from Web Audio: the timeline reaches the engine when it is set
 * or scheduled and then on each control tick, not per sample (see the file
 * header); there is no `automationRate`; setting `.value` clears the whole
 * timeline; every value is clamped into [minValue, maxValue]. A ramp with no
 * earlier event starts from `.value`'s stored base at the call's
 * `currentTime`.
 */
class AudioParam {

  /**
   * Reading returns the timeline evaluated at `currentTime` (the stored base
   * when nothing is scheduled). It pushes nothing to the engine and does not
   * write the evaluated value back, so the base a later ramp starts from is
   * unchanged. Writing clears the timeline, sets the base and applies it to
   * the engine at once.
   * @type {number}
   */
  value;

  /** @readonly @type {number} */
  defaultValue;

  /** @readonly @type {number} */
  minValue;

  /** @readonly @type {number} */
  maxValue;

  /**
   * @param {number} value
   * @param {number} [startTime=0]
   * @returns {AudioParam} this
   */
  setValueAtTime(value, startTime) {}

  /**
   * @param {number} value
   * @param {number} [endTime=0]
   * @returns {AudioParam} this
   */
  linearRampToValueAtTime(value, endTime) {}

  /**
   * Falls back to a linear ramp when either end is not positive.
   * @param {number} value
   * @param {number} [endTime=0]
   * @returns {AudioParam} this
   */
  exponentialRampToValueAtTime(value, endTime) {}

  /**
   * A time constant of 0 jumps straight to `target`.
   * @param {number} target
   * @param {number} [startTime=0]
   * @param {number} [timeConstant=0]
   * @returns {AudioParam} this
   */
  setTargetAtTime(target, startTime, timeConstant) {}

  /**
   * `values` may be a Float32Array or a plain array of numbers.
   * @param {Float32Array|Array<number>} values
   * @param {number} [startTime=0]
   * @param {number} [duration=0]
   * @returns {AudioParam} this
   */
  setValueCurveAtTime(values, startTime, duration) {}

  /**
   * Drops every event at or after `cancelTime`.
   * @param {number} cancelTime
   * @returns {AudioParam} this
   */
  cancelScheduledValues(cancelTime) {}

  /**
   * @param {number} cancelTime
   * @returns {AudioParam} this
   */
  cancelAndHoldAtTime(cancelTime) {}

  /**
   * Not in Web Audio: the timeline's value at engine time `time`, without
   * applying it.
   * @param {number} [time=0]
   * @returns {number}
   */
  getValueAtTime(time) {}

}

/**
 * Base of every node class below. `connect`/`disconnect` record the graph
 * (read by a source's `start()`) and switch master-bus effects on and off as
 * the file header describes. Output/input indices are ignored.
 */
class AudioNode {

  /**
   * Records `destination` and returns it, so calls chain. TypeError when
   * `destination` is missing, undefined, or not an AudioNode or AudioParam.
   * An AudioParam destination is recorded but has no effect (there is no
   * audio-rate modulation). Side effects: a BiquadFilterNode's own connect()
   * enables its filter slot (connecting something INTO the filter does not);
   * a DelayNode / DynamicsCompressorNode / WaveShaperNode / ConvolverNode on
   * either end enables the master delay (time = delayTime at `currentTime`,
   * mix 1) / compressor (the node's params, converted as described under
   * DynamicsCompressorNode) / distortion (soft clip, mix 1) / reverb (mix 1);
   * a MediaStreamAudioSourceNode connected to an AnalyserNode sets its
   * `source` to 1 (microphone).
   * @param {AudioNode|AudioParam} destination
   * @returns {AudioNode|AudioParam} destination
   */
  connect(destination) {}

  /**
   * With no argument, forgets every destination; with one, forgets that
   * destination. Either way a BiquadFilterNode disables its slot and a
   * Delay / Compressor / WaveShaper / Convolver node disables the matching
   * master-bus effect (even if another such node is still connected).
   * @param {AudioNode|AudioParam} [destination]
   */
  disconnect(destination) {}

  /** 0 for sources, the input count for a ChannelMergerNode, else 1. @readonly @type {number} */
  numberOfInputs;

  /** 0 for the destination, the output count for a ChannelSplitterNode, else 1. @readonly @type {number} */
  numberOfOutputs;

  /** Always 2 (a splitter reports its output count, a merger 1); assignment is ignored. @type {number} */
  channelCount;

  /** 'max' ('explicit' on splitter/merger). Informational. @type {string} */
  channelCountMode;

  /** 'speakers' ('discrete' on a splitter). Informational. @type {string} */
  channelInterpretation;

}

/** `ctx.destination`: the master output. Not constructible. */
class AudioDestinationNode extends AudioNode {

  /** Always 2. @readonly @type {number} */
  maxChannelCount;

}

/**
 * A synth voice. Created voices are engine voices (`voiceId`), so the voice
 * API on AudioContext (`setVoiceBus`, `setVoiceSpatialPosition`, ... in
 * `audio-engine-api.js`) works on them too. Plays on `start()` whether or
 * not it is connected.
 */
class OscillatorNode extends AudioNode {

  /**
   * 'sine' | 'square' | 'sawtooth' | 'triangle' | 'custom' (a PeriodicWave;
   * also 'wavetable') | 'whitenoise' | 'pinknoise' | 'brownnoise'. Applied to
   * the voice immediately. An unknown name plays as sine but reads back as
   * written.
   * @type {string}
   */
  type;

  /** Hz, default 440, range 0..24000. Live. @readonly @type {AudioParam} */
  frequency;

  /**
   * Cents, default 0. Live: the voice plays at
   * frequency * 2^(detune / 1200).
   * @readonly @type {AudioParam}
   */
  detune;

  /**
   * Not in Web Audio. Voice pan, -1..1, default 0. Live. Added to a
   * StereoPannerNode's pan on the path, the sum clamped to -1..1.
   * @readonly @type {AudioParam}
   */
  pan;

  /** Not in Web Audio. Envelope attack, seconds (0..60, default 0.01). Live. @readonly @type {AudioParam} */
  attack;

  /** Not in Web Audio. Envelope decay, seconds (0..60, default 0.1). Live. @readonly @type {AudioParam} */
  decay;

  /** Not in Web Audio. Envelope sustain level (0..1, default 1). Live. @readonly @type {AudioParam} */
  sustain;

  /** Not in Web Audio. Envelope release, seconds (0..60, default 0.04). Live. @readonly @type {AudioParam} */
  release;

  /** Not in Web Audio. Semitones, -24..24, default 0. Live. @readonly @type {AudioParam} */
  pitchBend;

  /**
   * Not in Web Audio. The voice's own gain, 0..10, default 1. Live. The
   * voice plays at this gain multiplied by every GainNode's gain on its path
   * (all of them, each live).
   * @readonly @type {AudioParam}
   */
  gain;

  /** Not in Web Audio. The engine voice id (-1 without an engine). @readonly @type {number} */
  voiceId;

  /**
   * Starts the voice at `when` (engine seconds; default now). First walks the
   * connected graph once: every GainNode's gain joins the voice's gain
   * product, a StereoPannerNode's pan joins its pan, a PannerNode turns on
   * spatialization (its position stays live; refDistance / maxDistance /
   * rolloffFactor / distanceModel are copied now), and Delay / Compressor /
   * WaveShaper / Convolver nodes enable their master-bus effect. A DelayNode
   * or DynamicsCompressorNode reached this way is bound to the master bus,
   * so its params stay live from then on. Throws Error on a second call and
   * RangeError for a negative or NaN `when` (a rejected call does not count
   * as the start).
   * @param {number} [when]
   */
  start(when) {}

  /**
   * Releases the voice at `when` (engine seconds; default now). RangeError
   * for a negative or NaN `when`. No `onended` is fired for an oscillator.
   * @param {number} [when]
   */
  stop(when) {}

  /**
   * Switches the voice to the wave's wavetable and `type` to 'custom'.
   * TypeError for anything that is not a PeriodicWave.
   * @param {PeriodicWave} wave
   */
  setPeriodicWave(wave) {}

}

/**
 * A gain on a source's path. Every source started with this node downstream
 * multiplies its gain by `gain`, live: `.value` writes and automation reach
 * the playing sources (see the file header). Several GainNodes on one path
 * multiply. A source started before the node was connected is not affected.
 */
class GainNode extends AudioNode {

  /** Default 1, unbounded. Live. @readonly @type {AudioParam} */
  gain;

}

/**
 * A band-limited wavetable built from Fourier coefficients (cosine terms in
 * `real`, sine terms in `imag`, index 0 = DC) at the engine sample rate.
 *
 * Two constructor forms: Web Audio's `new PeriodicWave(ctx, {real, imag,
 * disableNormalization})`, recognised by an AudioContext first argument, and
 * a positional `new PeriodicWave(real, imag, options)`. Each half is a
 * Float32Array or a plain array of numbers; a missing, undefined or null half
 * is empty, and anything else (another typed array, a detached one, an
 * ArrayBuffer) is a TypeError. Unlike Web Audio, the halves may differ in
 * length: the shorter is zero-padded. No properties or methods.
 */
class PeriodicWave {

  /**
   * @param {AudioContext|Float32Array|Array<number>} [ctxOrReal]
   * @param {PeriodicWaveOptions|Float32Array|Array<number>} [optionsOrImag]
   * @param {PeriodicWaveOptions} [options]  Positional form only.
   */
  constructor(ctxOrReal, optionsOrImag, options) {}

}

/**
 * One of the master bus's 4 filter slots. Creating the node allocates the
 * slot (lowpass, 350 Hz, Q 1, 0 dB) but leaves it OFF, so creating a filter
 * does not change the sound. The filter node's own `connect()` switches the
 * slot on, and it then filters the whole mix; `disconnect()` switches it off
 * again. Throws Error("No filter slots available") when all 4 are taken;
 * garbage collection releases the slot. For per-bus or per-voice filtering
 * use the bus filter API or `setVoiceFilter*`.
 */
class BiquadFilterNode extends AudioNode {

  /**
   * 'lowpass' | 'highpass' | 'bandpass' | 'notch' | 'allpass' | 'peaking' |
   * 'lowshelf' | 'highshelf'. Live; unknown names act as lowpass.
   * @type {string}
   */
  type;

  /**
   * Hz, default 350. Live. The slot's cutoff is
   * frequency * 2^(detune / 1200), which the engine clamps to 20..20000.
   * @readonly @type {AudioParam}
   */
  frequency;

  /** Cents, default 0. Live: shifts the slot's cutoff, as above. @readonly @type {AudioParam} */
  detune;

  /** Default 1 (the engine clamps to 0.1..30). Live. @readonly @type {AudioParam} */
  Q;

  /** dB, default 0, -40..40 (peaking / shelf types). Live. @readonly @type {AudioParam} */
  gain;

  /**
   * RBJ-cookbook response of the params at `currentTime` (automation and
   * detune included). `frequencyHz` is a Float32Array or a plain array of
   * numbers; the two outputs must be Float32Arrays. TypeError otherwise,
   * including for a detached array. Writes min(lengths) entries.
   * @param {Float32Array|Array<number>} frequencyHz
   * @param {Float32Array} magResponse
   * @param {Float32Array} phaseResponse
   */
  getFrequencyResponse(frequencyHz, magResponse, phaseResponse) {}

}

/**
 * Reads the latest samples of the engine's mono output, the microphone, or
 * both, and runs a Blackman-windowed FFT on demand. The float `get*` methods
 * take a Float32Array and the byte methods a Uint8Array; anything else
 * (no argument, a plain array, another element type, a detached array)
 * throws TypeError.
 *
 * Differs from Web Audio: what it measures is picked by `source`, not by what
 * is connected to it. An AudioBufferSourceNode upstream writes its processed
 * buffer into the analyser once, at `start()`, and the analyser then reads
 * the tail of that buffer rather than the live output. Any other node
 * connected to it makes it read that (empty) tap, i.e. silence, until
 * disconnected.
 */
class AnalyserNode extends AudioNode {

  /**
   * Not in Web Audio. 0 = engine output (default), 1 = microphone ring
   * (needs capture running: getUserMedia or bro.mic), 2 = both summed (the
   * mic part only while `ctx.micMuted` is false). Other values read as 0.
   * @type {number}
   */
  source;

  /**
   * Power of two, 32..32768, default 2048; RangeError otherwise. The source
   * rings hold 16384 samples, so 32768 repeats data.
   * @type {number}
   */
  fftSize;

  /** fftSize / 2. @readonly @type {number} */
  frequencyBinCount;

  /** Default -100. Maps dB to the byte range. @type {number} */
  minDecibels;

  /** Default -30. @type {number} */
  maxDecibels;

  /** 0..1 (clamped), default 0.8. Shared by the float and byte frequency reads. @type {number} */
  smoothingTimeConstant;

  /** @param {Float32Array} array  Receives up to frequencyBinCount dB values. */
  getFloatFrequencyData(array) {}

  /** @param {Uint8Array} array  Receives up to frequencyBinCount values, 0..255 over [minDecibels, maxDecibels]. */
  getByteFrequencyData(array) {}

  /** @param {Float32Array} array  Receives up to fftSize samples. */
  getFloatTimeDomainData(array) {}

  /** @param {Uint8Array} array  Receives up to fftSize samples mapped -1..1 to 0..255. */
  getByteTimeDomainData(array) {}

}

/**
 * PCM data, one Float32 array per channel. Made by `ctx.createBuffer`,
 * `new AudioBuffer({...})` or `ctx.decodeAudioData`.
 *
 * `sampleRate` is the rate the frames are at, and it is honoured: an
 * AudioBufferSourceNode plays the buffer at its own rate, resampled to the
 * context rate on the fly, and `createClip(buffer)` resamples it to the
 * engine rate. `createBuffer` and `new AudioBuffer` default it to the
 * context rate; decodeAudioData's buffers are already at that rate.
 *
 * Differs from Web Audio: `copyFromChannel` / `copyToChannel` silently
 * ignore a bad channel or start index instead of throwing. Their array must
 * be a Float32Array (TypeError otherwise, including for a detached one).
 */
class AudioBuffer {

  /**
   * @param {AudioBufferOptions} options
   */
  constructor(options) {}

  /** @readonly @type {number} */
  numberOfChannels;

  /** Frames. @readonly @type {number} */
  length;

  /** @readonly @type {number} */
  sampleRate;

  /** length / sampleRate, in seconds. @readonly @type {number} */
  duration;

  /**
   * The channel's Float32Array. The same array is returned on every call,
   * and writes into it are what `start()`, `createClip` and
   * `copyFromChannel` read. RangeError for a bad index.
   * @param {number} channel
   * @returns {Float32Array}
   */
  getChannelData(channel) {}

  /**
   * @param {Float32Array} destination
   * @param {number} channelNumber
   * @param {number} [startInChannel=0]
   */
  copyFromChannel(destination, channelNumber, startInChannel) {}

  /**
   * @param {Float32Array} source
   * @param {number} channelNumber
   * @param {number} [startInChannel=0]
   */
  copyToChannel(source, channelNumber, startInChannel) {}

}

/**
 * Plays an AudioBuffer as an engine clip. `start()` interleaves the buffer,
 * applies the downstream graph to the samples (biquad at the slot's current
 * settings, DelayNode as a shift inside the buffer's length, ConvolverNode
 * with its impulse, WaveShaperNode curve, DynamicsCompressorNode, and feeds
 * any AnalyserNode), then plays the result. The gain product, stereo pan and
 * PannerNode position found on the way stay live (see the file header).
 * Plays whether or not it is connected. The buffer plays at its own
 * `sampleRate`, resampled to the context rate.
 *
 * While it plays, the binding holds the node (it is not collected), and
 * `playbackRate`, `detune`, `loop`, `loopStart` and `loopEnd` are live.
 *
 * Differs from Web Audio: `stop()` ignores its time and stops at once; the
 * playback id is not exposed, so the engine's setPlayback* methods cannot
 * address it.
 */
class AudioBufferSourceNode extends AudioNode {

  /**
   * An AudioBuffer, or null (the initial value). Assigning null or undefined
   * clears it; assigning anything that is not an AudioBuffer throws
   * TypeError. Read at `start()`.
   * @type {AudioBuffer|null}
   */
  buffer;

  /** Live while playing. @type {boolean} */
  loop;

  /**
   * Seconds into the buffer, default 0. With `loopEnd`, the loop window;
   * live while playing (moving either end moves the playing window). The
   * window applies only when 0 <= loopStart < loopEnd; each end is clamped to
   * the buffer's length. Otherwise (including the default loopEnd of 0) the
   * whole buffer loops, and loopStart is not used.
   * @type {number}
   */
  loopStart;

  /** Seconds into the buffer, default 0. See loopStart. @type {number} */
  loopEnd;

  /**
   * Default 1, range 0..1024 (the engine clamps the resulting rate to
   * 0.01..16). Live.
   * @readonly @type {AudioParam}
   */
  playbackRate;

  /**
   * Cents, default 0. Live: the playback rate is
   * playbackRate * 2^(detune / 1200), times the buffer-to-context rate ratio.
   * @readonly @type {AudioParam}
   */
  detune;

  /**
   * Called once the source ends: when the playback finishes (the buffer or
   * `duration` runs out, which a loop without `duration` never does) or
   * `stop()` is called. It runs on the host's frame tick after the end (or
   * at the end of a `ctx.renderBlock()`), not on the audio thread. It is
   * called with `this` = the node and one event
   * `{ type: 'ended', target: node, currentTarget: node }`. Only this
   * property is read; there is no addEventListener. A source that was never
   * started, or started without a buffer (or with an empty one), never ends
   * and never fires it.
   * @type {function({type: string, target: AudioBufferSourceNode, currentTarget: AudioBufferSourceNode}): void|null}
   */
  onended;

  /**
   * Web Audio's `start(when, offset, duration)`, all in seconds:
   * `when` on the context clock (0, or any time already past, plays now;
   * later times are scheduled sample-accurately on the audio clock),
   * `offset` into the buffer (in buffer time, clamped to its length), and
   * `duration` of buffer content to play before the source ends, counting
   * content played across loop wraps (so it is measured in buffer time, not
   * wall time when playbackRate is not 1). Without `duration` a
   * non-looping source plays to the buffer's end and a looping one plays
   * until stopped. RangeError when any argument is negative or NaN; Error on
   * a second call. Does nothing audible without a buffer.
   * @param {number} [when=0]
   * @param {number} [offset=0]
   * @param {number} [duration]
   */
  start(when, offset, duration) {}

  /**
   * Stops immediately (any argument is ignored); `onended` follows on the
   * next tick.
   */
  stop() {}

}

/**
 * Spatializes a source started with it downstream. The position params are
 * live: `.value` writes, `setPosition()` and automation move a playing
 * source. The distance settings (refDistance, maxDistance, rolloffFactor,
 * distanceModel) are copied onto the source at `start()`; changing them
 * afterwards does not affect it. The engine uses its own head model
 * (`setHeadModel*`) whatever `panningModel` says, and ignores orientation and
 * the cone settings.
 */
class PannerNode extends AudioNode {

  /** 'equalpower' (default) | 'HRTF'. Stored only; other values are ignored. @type {string} */
  panningModel;

  /** 'inverse' (default) | 'linear' | 'exponential'; other values are ignored. @type {string} */
  distanceModel;

  /** Default 1. @type {number} */
  refDistance;

  /** Default 10000. @type {number} */
  maxDistance;

  /** Default 1. @type {number} */
  rolloffFactor;

  /** Default 360. Stored only. @type {number} */
  coneInnerAngle;

  /** Default 360. Stored only. @type {number} */
  coneOuterAngle;

  /** Default 0. Stored only. @type {number} */
  coneOuterGain;

  /** Default 0. Live. @readonly @type {AudioParam} */
  positionX;
  /** Default 0. Live. @readonly @type {AudioParam} */
  positionY;
  /** Default 0. Live. @readonly @type {AudioParam} */
  positionZ;
  /** Default 1. Stored only. @readonly @type {AudioParam} */
  orientationX;
  /** Default 0. Stored only. @readonly @type {AudioParam} */
  orientationY;
  /** Default 0. Stored only. @readonly @type {AudioParam} */
  orientationZ;

  /**
   * Sets positionX/Y/Z. Needs all three arguments.
   * @param {number} x
   * @param {number} y
   * @param {number} z
   */
  setPosition(x, y, z) {}

  /**
   * Sets orientationX/Y/Z. Needs all three arguments.
   * @param {number} x
   * @param {number} y
   * @param {number} z
   */
  setOrientation(x, y, z) {}

}

/**
 * Pans a source started with it downstream left/right, live. For an
 * oscillator the pan is added to the voice's own `pan` param (the sum
 * clamped to -1..1).
 */
class StereoPannerNode extends AudioNode {

  /**
   * -1..1, default 0. Live. Set it through `pan.value`: assigning a number to
   * `pan` itself replaces the param and has no effect.
   * @readonly @type {AudioParam}
   */
  pan;

}

/**
 * Drives the master bus delay (see the file header). `delayTime` is applied
 * at `connect()` and when an OscillatorNode's `start()` reaches the node.
 * That start also binds the node to the master bus: from then on its
 * `delayTime` is live (`.value` writes and automation reach the bus). A
 * buffer source's start does not bind it. The engine clamps the time to
 * 0.001..2 s and uses its own feedback setting (`setDelayFeedback`).
 * For an AudioBufferSourceNode the buffer is also shifted by the delay at
 * `start()` (dropped past the buffer's end).
 */
class DelayNode extends AudioNode {

  /**
   * @param {number} [maxDelayTime=1]  Seconds; <= 0 means 1, capped at 180.
   */
  constructor(maxDelayTime) {}

  /** Seconds, default 0, range 0..maxDelayTime. @readonly @type {AudioParam} */
  delayTime;

}

/**
 * Drives the master bus compressor (see the file header), and compresses an
 * AudioBufferSourceNode's samples at `start()` with the Web Audio parameter
 * meanings below. The params are applied to the bus at `connect()` and when
 * an OscillatorNode's `start()` reaches the node; that start also binds the
 * node to the master bus, and from then on threshold, ratio, attack and
 * release are live (`.value` writes and automation). A buffer source's start
 * does not bind it.
 *
 * Unit conversion onto the bus: `threshold` dB becomes the bus's linear
 * level (10^(dB/20), so -24 dB is about 0.063); attack and release seconds
 * become milliseconds. The bus clamps attack to 0.1..100 ms, so an `attack`
 * above 0.1 s acts as 0.1 s on the bus (the buffer-source pass uses the
 * full value), and release to 1..1000 ms.
 */
class DynamicsCompressorNode extends AudioNode {

  /** dB, default -24, -100..0. @readonly @type {AudioParam} */
  threshold;

  /** dB, default 30, 0..40. Buffer-source processing only: the bus has no knee. @readonly @type {AudioParam} */
  knee;

  /** Default 12, 1..20. @readonly @type {AudioParam} */
  ratio;

  /** Seconds, default 0.003, 0..1. The bus caps it at 0.1 s. @readonly @type {AudioParam} */
  attack;

  /** Seconds, default 0.25, 0..1. @readonly @type {AudioParam} */
  release;

  /**
   * dB of gain reduction left by the last buffer-source pass (0 until one
   * has run). It does not follow the live master compressor.
   * @readonly @type {number}
   */
  reduction;

}

/**
 * Drives the master bus distortion in soft-clip mode (see the file header).
 * `curve` is applied only to AudioBufferSourceNode samples at `start()`.
 */
class WaveShaperNode extends AudioNode {

  /**
   * A copy of the curve, or null when unset. Assigning copies a Float32Array
   * or plain array of numbers in; null or undefined clears it; anything else
   * (another typed array, a detached one) throws TypeError.
   * @type {Float32Array|null}
   */
  curve;

  /** 'none' (default) | '2x' | '4x'. Stored only; other values are ignored. @type {string} */
  oversample;

}

/**
 * Drives the master bus's algorithmic reverb (see the file header); the
 * impulse response is used only to convolve AudioBufferSourceNode samples at
 * `start()` (a mono source comes out stereo; more than 2 channels pass
 * through unconvolved).
 */
class ConvolverNode extends AudioNode {

  /**
   * The impulse response, or null (the initial value). Assigning null or
   * undefined clears it; anything that is not an AudioBuffer throws TypeError.
   * @type {AudioBuffer|null}
   */
  buffer;

  /** Default true. @type {boolean} */
  normalize;

}

/**
 * Placeholder for graph compatibility: routes nothing, but the `start()`
 * graph walk passes through it to the nodes behind it.
 */
class ChannelSplitterNode extends AudioNode {

  /** @param {number} [numberOfOutputs=6]  Clamped to 1..32 (<= 0 means 6). */
  constructor(numberOfOutputs) {}

  /** @readonly @type {number} */
  numberOfOutputs;

}

/**
 * Placeholder for graph compatibility, like ChannelSplitterNode.
 */
class ChannelMergerNode extends AudioNode {

  /** @param {number} [numberOfInputs=6]  Clamped to 1..32 (<= 0 means 6). */
  constructor(numberOfInputs) {}

  /** @readonly @type {number} */
  numberOfInputs;

}

/**
 * What `navigator.mediaDevices.getUserMedia` resolves with. It stands for
 * "engine mic capture is running" and carries no tracks. `new MediaStream()`
 * works but does not start capture.
 */
class MediaStream {

  /** Always true. @readonly @type {boolean} */
  active;

}

/**
 * The microphone as a graph node. It carries no audio through the graph: its
 * only effect is that connecting it to an AnalyserNode sets the analyser's
 * `source` to 1. Hearing the mic is `ctx.micMuted` / `micMonitorGain` /
 * `micBus`.
 */
class MediaStreamAudioSourceNode extends AudioNode {}

/**
 * The listener, `ctx.listener`: a plain object, not a class instance. The
 * methods set the engine's listener, which every spatialized voice and
 * playback is heard from.
 *
 * Differs from Web Audio: the position/forward/up AudioParams exist but are
 * not wired to the engine; use the methods.
 */
class AudioListener {

  /** @param {number} x @param {number} y @param {number} z */
  setPosition(x, y, z) {}

  /**
   * Forward vector then up vector. Needs all six arguments.
   * @param {number} fx @param {number} fy @param {number} fz
   * @param {number} ux @param {number} uy @param {number} uz
   */
  setOrientation(fx, fy, fz, ux, uy, uz) {}

  /** Used for Doppler. @param {number} x @param {number} y @param {number} z */
  setVelocity(x, y, z) {}

  /** Same as setPosition. @param {number} x @param {number} y @param {number} z */
  setListenerPosition(x, y, z) {}

  /** Same as setOrientation. */
  setListenerOrientation(fx, fy, fz, ux, uy, uz) {}

  /** Default 0. Not wired. @readonly @type {AudioParam} */
  positionX;
  /** Default 0. Not wired. @readonly @type {AudioParam} */
  positionY;
  /** Default 0. Not wired. @readonly @type {AudioParam} */
  positionZ;
  /** Default 0, -1..1. Not wired. @readonly @type {AudioParam} */
  forwardX;
  /** Default 0, -1..1. Not wired. @readonly @type {AudioParam} */
  forwardY;
  /** Default -1, -1..1. Not wired. @readonly @type {AudioParam} */
  forwardZ;
  /** Default 0, -1..1. Not wired. @readonly @type {AudioParam} */
  upX;
  /** Default 1, -1..1. Not wired. @readonly @type {AudioParam} */
  upY;
  /** Default 0, -1..1. Not wired. @readonly @type {AudioParam} */
  upZ;

}

/**
 * Entry point. `webkitAudioContext` is the same constructor. Takes no
 * options (any argument is ignored). See the file header for how contexts
 * share the one engine. The engine methods (clips, playbacks, streams,
 * voices, wavetables, buses, master bus, head model, offline rendering,
 * presets) are in `audio-engine-api.js`.
 */
class AudioContext {

  constructor() {}

  // ── Properties ──────────────────────────────────────────────────────────

  /** Engine time in seconds; shared by every context. @readonly @type {number} */
  currentTime;

  /** The engine sample rate. @readonly @type {number} */
  sampleRate;

  /** 'running' | 'suspended' | 'closed'. @readonly @type {string} */
  state;

  /** Device-buffer latency estimate in seconds (0 headless). @readonly @type {number} */
  outputLatency;

  /** Always 0. @readonly @type {number} */
  baseLatency;

  /** @readonly @type {AudioDestinationNode} */
  destination;

  /** @readonly @type {AudioListener} */
  listener;

  /** Not in Web Audio. Engine master gain, default 0.5, clamped 0..2. @type {number} */
  masterGain;

  /** Not in Web Audio. Whether startRecording is active. @readonly @type {boolean} */
  recording;

  /** Not in Web Audio. Doppler scale: 0 disables, 1 physical (default), > 1 exaggerates. @type {number} */
  dopplerFactor;

  /**
   * Not in Web Audio. Default true: captured mic audio is not played to the
   * output. Analysis and `bro.mic` taps see the mic either way.
   * @type {boolean}
   */
  micMuted;

  /** Not in Web Audio. Mic monitor level when unmuted, default 0.5. @type {number} */
  micMonitorGain;

  /** Not in Web Audio. Bus the mic monitor goes to; -1 (default) = straight to the output. @type {number} */
  micBus;

  // ── Lifecycle ───────────────────────────────────────────────────────────

  /**
   * Pauses the engine clock: every voice, clip, schedule and `currentTime`
   * freezes (all contexts). No-op once closed.
   * @returns {Promise<void>}  already resolved
   */
  suspend() {}

  /** Unpauses the engine. No-op once closed. @returns {Promise<void>} */
  resume() {}

  /**
   * Stops and removes this context's oscillator voices, stops mic capture
   * and recording, and pauses the engine.
   * @returns {Promise<void>}
   */
  close() {}

  // ── Node factories ──────────────────────────────────────────────────────

  /** @returns {GainNode} */
  createGain() {}

  /** Allocates an engine voice (sine, 440 Hz). @returns {OscillatorNode} */
  createOscillator() {}

  /**
   * Each half is a Float32Array or a plain array of numbers; anything else
   * (another typed array, a detached one, an ArrayBuffer) throws TypeError.
   * Unlike Web Audio the lengths may differ: the shorter half is
   * zero-padded to the longer. `options.disableNormalization` is read only
   * when it is a boolean.
   * @param {Float32Array|Array<number>} real
   * @param {Float32Array|Array<number>} imag
   * @param {PeriodicWaveOptions} [options]
   * @returns {PeriodicWave|null} null with fewer than two arguments
   */
  createPeriodicWave(real, imag, options) {}

  /** See BiquadFilterNode (throws when the 4 slots are taken). @returns {BiquadFilterNode} */
  createBiquadFilter() {}

  /** @returns {AnalyserNode} */
  createAnalyser() {}

  /** @returns {AudioBufferSourceNode} */
  createBufferSource() {}

  /**
   * Unlike Web Audio, a zero length does not throw.
   * @param {number} [numberOfChannels=1]  Clamped to 1..32.
   * @param {number} [length=0]  Frames.
   * @param {number} [sampleRate]  Default: the context rate. Honoured when
   *   the buffer plays (see AudioBuffer).
   * @returns {AudioBuffer}
   */
  createBuffer(numberOfChannels, length, sampleRate) {}

  /** @returns {PannerNode} */
  createPanner() {}

  /** @returns {StereoPannerNode} */
  createStereoPanner() {}

  /** @param {number} [maxDelayTime=1] @returns {DelayNode} */
  createDelay(maxDelayTime) {}

  /** @returns {DynamicsCompressorNode} */
  createDynamicsCompressor() {}

  /** @returns {WaveShaperNode} */
  createWaveShaper() {}

  /** @returns {ConvolverNode} */
  createConvolver() {}

  /** @param {number} [numberOfOutputs=6] @returns {ChannelSplitterNode} */
  createChannelSplitter(numberOfOutputs) {}

  /** @param {number} [numberOfInputs=6] @returns {ChannelMergerNode} */
  createChannelMerger(numberOfInputs) {}

  /**
   * TypeError("Expected MediaStream argument") for anything that is not a
   * MediaStream; undefined with no argument.
   * @param {MediaStream} stream
   * @returns {MediaStreamAudioSourceNode}
   */
  createMediaStreamSource(stream) {}

  /** See `audio-engine-api.js`. @param {number} [maxVoices=16] @returns {VoiceAllocator} */
  createVoiceAllocator(maxVoices) {}

  /** The engine-wide matrix; see `audio-engine-api.js`. @returns {ModMatrix} */
  createModMatrix() {}

  /** Same as createModMatrix. @returns {ModMatrix} */
  getModMatrix() {}

  /** See `audio-engine-api.js`. @returns {MidiInput|null} null without an engine */
  createMidiInput() {}

  /**
   * TypeError("Expected VoiceAllocator argument") for anything that is not a
   * VoiceAllocator; undefined with no argument. See `audio-engine-api.js`.
   * @param {VoiceAllocator} allocator
   * @returns {Sequence}
   */
  createSequence(allocator) {}

  // ── Decoding ────────────────────────────────────────────────────────────

  /**
   * Decodes WAV / FLAC / MP3 / Ogg bytes synchronously, resamples to the
   * engine rate, and returns an already-settled promise of an AudioBuffer.
   * The AudioBuffer (and the promise object itself, so
   * `ctx.decodeAudioData(bytes).samples` works without awaiting) also carry
   * the AudioDecodedBuffer fields. `successCallback` / `errorCallback` are
   * called before the promise settles. Failure rejects with an Error named
   * 'EncodingError' — except that bad or undecodable input with NO callbacks
   * returns null instead of a promise. No argument rejects with an Error
   * named 'TypeError'.
   * @param {ArrayBuffer|ArrayBufferView} audioData
   * @param {function(AudioBuffer): void} [successCallback]
   * @param {function(Error): void} [errorCallback]
   * @returns {Promise<AudioBuffer>|null}
   */
  decodeAudioData(audioData, successCallback, errorCallback) {}

  /**
   * Decodes a file synchronously (resampled to the engine rate).
   * @param {string} path
   * @returns {AudioDecodedBuffer|null} null when missing or undecodable
   */
  decodeAudioFile(path) {}

  // ── Recording & WAV ─────────────────────────────────────────────────────

  /** Starts capturing the engine's mono output mix. */
  startRecording() {}

  /**
   * Stops and returns the capture: mono samples at the engine rate, at most
   * the last 2,646,000 (60 s at 44.1 kHz).
   * @returns {Float32Array|null} null when nothing was recorded
   */
  stopRecording() {}

  /**
   * Writes the last recording (call after stopRecording) as WAV.
   * @param {string} path
   * @returns {boolean}
   */
  exportRecordingToWav(path) {}

  /**
   * Writes interleaved float samples as a 16-bit WAV. Needs all four
   * arguments (false otherwise). TypeError when `samples` is not a
   * Float32Array (another element type or a detached array included); false
   * when channels or sampleRate is not positive or the file cannot be
   * written.
   * @param {string} path
   * @param {Float32Array} samples
   * @param {number} channels
   * @param {number} sampleRate
   * @returns {boolean}
   */
  saveWav(path, samples, channels, sampleRate) {}

}

// ── Microphone ───────────────────────────────────────────────────────────────

/**
 * Starts engine mic capture (default device) and resolves with a MediaStream
 * for `ctx.createMediaStreamSource`. The constraints are ignored. Rejects
 * with Error("Failed to access microphone") when capture cannot start.
 *
 * Capture feeds the mic analysis ring (AnalyserNode `source` 1 / 2) and every
 * `bro.mic` tap. It is not heard unless `ctx.micMuted = false` (then at
 * `ctx.micMonitorGain`, on `ctx.micBus`). `ctx.close()` stops capture.
 * The same function is also the global `__nativeGetUserMedia`.
 *
 * For fixed-size PCM chunks at a chosen rate (speech, ML, level meters) use
 * `bro.mic` instead: `bro.mic.start({ chunkFrames, targetRate, agc, live,
 * samples, onChunk, ... })`, `stop`, `isActive`, `engineRate`, `stats`,
 * `levels`, `feed` and `drain` (deliver pending chunks now, instead of
 * waiting for the once-per-frame drain). See `mic-api.js`.
 *
 * @example
 *   const ctx = new AudioContext();
 *   const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
 *   const an = ctx.createAnalyser();
 *   ctx.createMediaStreamSource(stream).connect(an);   // an.source becomes 1
 *   const buf = new Float32Array(an.fftSize);
 *   an.getFloatTimeDomainData(buf);
 *
 * @param {MediaStreamConstraints} [constraints]
 * @returns {Promise<MediaStream>}
 */
navigator.mediaDevices.getUserMedia = function(constraints) {};
