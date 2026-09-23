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
 * - THE GRAPH IS READ ONCE, AT start(). `start()` walks the nodes downstream
 *   of the source and copies what it finds onto the voice / playback: the
 *   GainNode gain(s), the StereoPannerNode pan, the PannerNode
 *   position and distance settings. Changing those nodes afterwards does not
 *   touch a source that is already playing.
 * - EFFECT NODES ARE MASTER-BUS EFFECTS. BiquadFilterNode owns one of the
 *   master bus's 4 filter slots; connecting anything to or from a DelayNode,
 *   DynamicsCompressorNode, WaveShaperNode or ConvolverNode switches on the
 *   master bus's delay, compressor, distortion (soft clip) or algorithmic
 *   reverb at 100% wet, and `disconnect()` switches it off again. They filter
 *   the whole mix, not only their input. For an AudioBufferSourceNode the
 *   same nodes are ALSO applied offline to the buffer at `start()` (biquad,
 *   delay shift, the real WaveShaper curve, the real Convolver impulse,
 *   compressor), so a buffer source routed through them is processed twice.
 * - AUTOMATION IS NOT SAMPLE-ACCURATE. AudioParam keeps a Web Audio timeline
 *   (set / linear / exponential / target / curve) but only pushes it to the
 *   engine when script touches the param: reading `.value`, or calling any
 *   scheduling method, evaluates the timeline at `currentTime` and applies
 *   the result. A ramp therefore advances only as fast as you poll it.
 *   Params that no engine object backs (GainNode.gain, DelayNode.delayTime,
 *   StereoPannerNode.pan, PannerNode position/orientation, the compressor
 *   params) are read at `start()` / `connect()` time only.
 * - NO EVENTS. There is no `onended`, `onstatechange`, AudioWorklet,
 *   ScriptProcessorNode, OfflineAudioContext, MediaElementSource,
 *   ConstantSourceNode or IIRFilterNode.
 * - GARBAGE COLLECTION STOPS SOUND. A collected OscillatorNode stops and
 *   frees its voice; a collected AudioBufferSourceNode stops its playback and
 *   deletes its clip; a collected BiquadFilterNode releases its slot. Keep a
 *   reference to anything that should keep playing.
 * - Node constructors (`new GainNode()`, `new DelayNode(maxTime)`, ...) ignore
 *   a context argument and option dictionaries; the `ctx.create*` factories
 *   are the normal path. `AudioNode`, `AudioParam` and `AudioDestinationNode`
 *   exist for `instanceof` and throw TypeError when constructed.
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
 *   g.gain.value = 0.3;          // read at start(); later changes do not apply
 *   pan.pan.value = -0.5;
 *   osc.connect(g).connect(pan).connect(ctx.destination);
 *   osc.start();
 *   osc.stop(ctx.currentTime + 1);
 *   osc.gain.value = 0.1;        // the voice's own gain param IS live
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
 * The third argument of `createPeriodicWave` / `new PeriodicWave`.
 * @typedef {Object} PeriodicWaveOptions
 * @property {boolean} [disableNormalization=false] -  Keep the summed table's
 *   amplitude instead of scaling its peak to 1.
 */

/**
 * The option object of `new AudioBuffer(options)`.
 * @typedef {Object} AudioBufferOptions
 * @property {number} length -  Frames; must be positive (TypeError otherwise).
 * @property {number} [numberOfChannels=1] -  Clamped to 1..32.
 * @property {number} [sampleRate=44100] -  Stored only; see AudioBuffer.
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
 * Differences from Web Audio: the timeline reaches the engine only when
 * script reads `.value` or calls a scheduling method (see the file header);
 * there is no `automationRate`; setting `.value` clears the whole timeline;
 * every value is clamped into [minValue, maxValue]. A ramp with no earlier
 * event starts from the current value at the call's `currentTime`.
 */
class AudioParam {

  /**
   * Reading evaluates the timeline at `currentTime` and pushes the result to
   * the engine; writing clears the timeline and sets the value.
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
   * Records `destination` and returns it, so calls chain. TypeError with no
   * argument. Side effects: a BiquadFilterNode enables its filter slot; a
   * DelayNode / DynamicsCompressorNode / WaveShaperNode / ConvolverNode on
   * either end enables the master delay (time = delayTime.value, mix 1) /
   * compressor / distortion (soft clip, mix 1) / reverb (mix 1); a
   * MediaStreamAudioSourceNode connected to an AnalyserNode sets its
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

  /** Cents, default 0. Stored only: it has no effect on the voice. @readonly @type {AudioParam} */
  detune;

  /** Not in Web Audio. Voice pan, -1..1, default 0. Live. @readonly @type {AudioParam} */
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
   * Not in Web Audio. The voice's own gain, 0..10, default 1. Live. `start()`
   * overwrites the voice gain (not this param's value) with a downstream
   * GainNode's gain when there is one.
   * @readonly @type {AudioParam}
   */
  gain;

  /** Not in Web Audio. The engine voice id (-1 without an engine). @readonly @type {number} */
  voiceId;

  /**
   * Starts the voice at `when` (engine seconds; default now). First walks the
   * connected graph: a GainNode's gain (evaluated at `when`) becomes the voice
   * gain (with several GainNodes the last one reached wins; they are not
   * multiplied, unlike for AudioBufferSourceNode),
   * a StereoPannerNode sets its pan, a PannerNode turns on spatialization at
   * the panner's position / refDistance / maxDistance / rolloffFactor /
   * distanceModel, and Delay / Compressor / WaveShaper / Convolver nodes
   * enable their master-bus effect. Throws Error on a second call.
   * @param {number} [when]
   */
  start(when) {}

  /**
   * Releases the voice at `when` (engine seconds; default now).
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
 * Holds a gain for `start()` to read. `gain` has no engine target: changing
 * it does not affect a source that is already playing (use the source's
 * `gain` param, `setVoiceGain` or `setPlaybackGain`).
 */
class GainNode extends AudioNode {

  /** Default 1, unbounded. @readonly @type {AudioParam} */
  gain;

}

/**
 * A band-limited wavetable built from Fourier coefficients (cosine terms in
 * `real`, sine terms in `imag`, index 0 = DC) at the engine sample rate.
 *
 * Differs from Web Audio: the constructor is positional,
 * `new PeriodicWave(real, imag, options)`; `new PeriodicWave(ctx, {real, imag})`
 * builds an empty wave. It accepts typed or plain arrays and zero-pads the
 * shorter half. No properties or methods.
 */
class PeriodicWave {

  /**
   * @param {Float32Array|Array<number>} [real]
   * @param {Float32Array|Array<number>} [imag]
   * @param {PeriodicWaveOptions} [options]
   */
  constructor(real, imag, options) {}

}

/**
 * One of the master bus's 4 filter slots. Creating the node allocates the
 * slot AND enables it (lowpass, 350 Hz, Q 1, 0 dB), so it filters the whole
 * mix as soon as it exists, connected or not; `disconnect()` disables it and
 * `connect()` re-enables it. Throws Error("No filter slots available") when
 * all 4 are taken; garbage collection releases the slot. For per-bus or
 * per-voice filtering use the bus filter API or `setVoiceFilter*`.
 */
class BiquadFilterNode extends AudioNode {

  /**
   * 'lowpass' | 'highpass' | 'bandpass' | 'notch' | 'allpass' | 'peaking' |
   * 'lowshelf' | 'highshelf'. Live; unknown names act as lowpass.
   * @type {string}
   */
  type;

  /** Hz, default 350 (the engine clamps to 20..20000). Live. @readonly @type {AudioParam} */
  frequency;

  /** Cents, default 0. Used only by getFrequencyResponse. @readonly @type {AudioParam} */
  detune;

  /** Default 1 (the engine clamps to 0.1..30). Live. @readonly @type {AudioParam} */
  Q;

  /** dB, default 0, -40..40 (peaking / shelf types). Live. @readonly @type {AudioParam} */
  gain;

  /**
   * RBJ-cookbook response of the current settings (detune included).
   * `frequencyHz` may be a plain array; the two outputs must be
   * Float32Arrays. Writes min(lengths) entries.
   * @param {Float32Array|Array<number>} frequencyHz
   * @param {Float32Array} magResponse
   * @param {Float32Array} phaseResponse
   */
  getFrequencyResponse(frequencyHz, magResponse, phaseResponse) {}

}

/**
 * Reads the latest samples of the engine's mono output, the microphone, or
 * both, and runs a Blackman-windowed FFT on demand. The `get*` methods do
 * nothing unless given a typed array; the float methods expect a
 * Float32Array.
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
 * Differs from Web Audio: `sampleRate` is stored but never used for
 * resampling, so a buffer played or turned into a clip is taken to be at the
 * engine rate (decodeAudioData already resamples to it).
 * `copyFromChannel` / `copyToChannel` silently ignore a bad channel or start
 * index instead of throwing, and expect Float32Arrays.
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
 * any AnalyserNode), then plays the result with the gain product, stereo pan
 * and PannerNode spatialization found on the way. Plays whether or not it is
 * connected.
 *
 * Differs from Web Audio: `loopStart`, `loopEnd` and the `duration` argument
 * of `start()` are ignored; `stop()` ignores its time and stops at once; the
 * playback id is not exposed, so a playing source cannot be moved or re-gained
 * afterwards (only `playbackRate` and `loop` stay live).
 */
class AudioBufferSourceNode extends AudioNode {

  /** An AudioBuffer, or null. @type {AudioBuffer|null} */
  buffer;

  /** Live after start(). @type {boolean} */
  loop;

  /** Stored only. @type {number} */
  loopStart;

  /** Stored only. @type {number} */
  loopEnd;

  /**
   * Default 1, range 0..1024 (the engine clamps to 0.01..16). Bound to the
   * playback after start(), so `.value` writes are live.
   * @readonly @type {AudioParam}
   */
  playbackRate;

  /** Cents, default 0. Folded into the rate once, at start(). @readonly @type {AudioParam} */
  detune;

  /**
   * `when` > 0 schedules the start on the audio clock (engine seconds);
   * otherwise it plays now. `offset` seeks into the buffer. Throws Error on
   * a second call. Does nothing audible without a buffer.
   * @param {number} [when=0]
   * @param {number} [offset=0]  Seconds.
   */
  start(when, offset) {}

  /** Stops immediately (any argument is ignored). */
  stop() {}

}

/**
 * Spatializes a source. Its settings are copied onto the source at
 * `start()` (the position params are evaluated at the start time); moving
 * the panner afterwards does not move a playing source. For a moving
 * source use `setVoiceSpatialPosition` / `setPlaybackSpatialPosition`.
 * The engine uses its own head model (`setHeadModel*`) whatever
 * `panningModel` says, and ignores orientation and the cone settings.
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

  /** Default 0. @readonly @type {AudioParam} */
  positionX;
  /** Default 0. @readonly @type {AudioParam} */
  positionY;
  /** Default 0. @readonly @type {AudioParam} */
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
 * Pans a source left/right. Read at `start()`.
 */
class StereoPannerNode extends AudioNode {

  /**
   * -1..1, default 0. Set it through `pan.value`: assigning a number to
   * `pan` itself replaces the param and has no effect.
   * @readonly @type {AudioParam}
   */
  pan;

}

/**
 * Drives the master bus delay (see the file header). `delayTime` is read at
 * `connect()` and at a source's `start()`; the engine clamps the time to
 * 0.001..2 s and uses its own feedback setting (`setDelayFeedback`).
 * For an AudioBufferSourceNode the buffer is also shifted by the delay
 * (dropped past the buffer's end).
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
 * meanings below. The params are read at `connect()` / `start()` only.
 */
class DynamicsCompressorNode extends AudioNode {

  /** dB, default -24, -100..0. @readonly @type {AudioParam} */
  threshold;

  /** dB, default 30, 0..40 (buffer-source processing only). @readonly @type {AudioParam} */
  knee;

  /** Default 12, 1..20. @readonly @type {AudioParam} */
  ratio;

  /** Seconds, default 0.003, 0..1. @readonly @type {AudioParam} */
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
   * or plain array in.
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

  /** The impulse response. @type {AudioBuffer|null} */
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
   * Both arrays must be typed arrays or ArrayBuffers (read as float32); the
   * coefficient count is the shorter length.
   * @param {Float32Array} real
   * @param {Float32Array} imag
   * @param {PeriodicWaveOptions} [options]
   * @returns {PeriodicWave|null} null with fewer than two arguments or a non-typed array
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
   * @param {number} [sampleRate=44100]
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
   * arguments (false otherwise). TypeError("Expected Float32Array as second
   * argument") for a non-typed-array `samples`; false when channels or
   * sampleRate is not positive or the file cannot be written.
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
