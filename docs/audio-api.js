// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * @typedef {Object} AudioDecodedBuffer
 * @property {Float32Array} [samples]
 * @property {number} [channels]
 * @property {number} [sampleRate]
 * @property {number} [numFrames]
 */

/**
 * @typedef {Object} StreamStats
 * @property {number} [decodedFrames]
 * @property {number} [playedFrames]
 * @property {number} [bufferedFrames]
 * @property {number} [underrunFrames]
 * @property {boolean} [finished]
 */

/**
 * @typedef {Object} StreamFromFileOptions
 * @property {number} [ringFrames]
 * @property {number} [prebufferFrames]
 * @property {boolean} [loop]
 * @property {number} [gain]
 */

/**
 * @typedef {Object} SequenceNote
 * @property {number} [beat] beat position (also available as `beatPosition`)
 * @property {number} [beatPosition]
 * @property {number} [note]
 * @property {number} [velocity]
 * @property {number} [duration]
 */

/**
 * Plain-object form of a broaudio VoicePreset: what `voicePresetToJson`
 * takes and `voicePresetFromJson` / `applyVoicePreset` give back. Every
 * field is optional; missing ones keep the struct default.
 * @typedef {Object} VoicePreset
 * @property {string} [waveform="sine"] sine | square | sawtooth | triangle | wavetable | ...
 * @property {number} [frequency=440]
 * @property {number} [gain=1]
 * @property {number} [pan=0]
 * @property {number} [pitchBend=0]
 * @property {number} [attackTime=0.01]
 * @property {number} [decayTime=0.1]
 * @property {number} [sustainLevel=1]
 * @property {number} [releaseTime=0.04]
 * @property {boolean} [filterEnabled=false]
 * @property {string} [filterType="lowpass"]
 * @property {number} [filterFreq=1000]
 * @property {number} [filterQ=1]
 * @property {number} [unisonCount=1]
 * @property {number} [unisonDetune=0.15]
 * @property {number} [unisonStereoWidth=0.7]
 */

/**
 * Plain-object form of a BusPreset (gain, filters, delay, compressor,
 * reverb, chorus, distortion, eq, and `effectOrder`: an array of slot
 * names as accepted by `setBusEffectOrder`).
 * @typedef {Object} BusPreset
 * @property {number} [gain=1]
 * @property {Array<Object>} [filters]
 * @property {Object} [delay]
 * @property {Object} [compressor]
 * @property {Object} [reverb]
 * @property {Object} [chorus]
 * @property {Object} [distortion]
 * @property {Object} [eq]
 * @property {Array<string>} [effectOrder]
 */

/**
 * Plain-object form of a ModPreset: `lfos` (shape, rate, depth, offset,
 * bipolar, sync) and `routes` (source, dest, amount, enabled).
 * @typedef {Object} ModPreset
 * @property {Array<Object>} [lfos]
 * @property {Array<Object>} [routes]
 */

/**
 * Plain-object form of an EnginePreset.
 * @typedef {Object} EnginePreset
 * @property {number} [masterGain=1]
 * @property {Object} [limiter]
 * @property {BusPreset} [masterBus]
 * @property {Array<BusPreset>} [buses]
 * @property {ModPreset} [modulation]
 */

/**
 * @typedef {Object} SequenceAutomationPoint
 * @property {number} [beat]
 * @property {number} [value]
 */

/**
 * @typedef {Object} MidiPort
 * @property {number} [index]
 * @property {string} [name]
 */

/**
 * @typedef {Object} MidiRawEvent
 * @property {string} [type]
 * @property {number} [channel]
 * @property {number} [data1]
 * @property {number} [data2]
 * @property {number} [pitchBend]
 * @property {number} [timestamp]
 */

/**
 * @typedef {Object} MediaStreamConstraints
 * @property {boolean} [audio]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

class AudioParam {

  /**
   * @type {number}
   */
  value;

  /**
   * @param {number} value
   * @param {number} time
   */
  setValueAtTime(value, time) {}

  /**
   * @param {number} value
   * @param {number} time
   */
  linearRampToValueAtTime(value, time) {}

  /**
   * @param {number} value
   * @param {number} time
   */
  exponentialRampToValueAtTime(value, time) {}

  /**
   * @param {number} target
   * @param {number} startTime
   * @param {number} timeConstant
   */
  setTargetAtTime(target, startTime, timeConstant) {}

  /**
   * @param {Float32Array} values
   * @param {number} startTime
   * @param {number} duration
   */
  setValueCurveAtTime(values, startTime, duration) {}

  /**
   * @param {number} cancelTime
   */
  cancelScheduledValues(cancelTime) {}

  /**
   * @param {number} cancelTime
   */
  cancelAndHoldAtTime(cancelTime) {}

}

class OscillatorNode {

  /**
   * @type {string}
   */
  type;

  /**
   * @readonly
   * @type {AudioParam}
   */
  frequency;

  /**
   * @readonly
   * @type {AudioParam}
   */
  detune;

  /**
   * Starts the voice at `when` (engine seconds, default: now). If the
   * oscillator is connected to a GainNode, that node's `gain.value` is
   * applied to the voice first, so `osc.connect(g); g.gain.value = 0.3;
   * osc.start()` plays at 0.3. Throws if called twice.
   * @param {number} [when=currentTime]
   */
  start(when) {}

  /**
   * @param {number} [when=currentTime]
   */
  stop(when) {}

  /**
   * Returns `destination`. A GainNode destination is remembered for
   * `start()` (see above); anything else is a pass-through.
   * @param {Object} destination
   */
  connect(destination) {}

  disconnect() {}

}

class GainNode {

  /**
   * @readonly
   * @type {AudioParam}
   */
  gain;

  /**
   * @param {Object} destination
   */
  connect(destination) {}

  disconnect() {}

}

class BiquadFilterNode {

  /**
   * @type {string}
   */
  type;

  /**
   * @readonly
   * @type {AudioParam}
   */
  frequency;

  /**
   * @readonly
   * @type {AudioParam}
   */
  detune;

  /**
   * @readonly
   * @type {AudioParam}
   */
  Q;

  /**
   * @readonly
   * @type {AudioParam}
   */
  gain;

  /**
   * Enables the node's master filter slot (the slot is allocated, enabled
   * and set to lowpass 350 Hz by createBiquadFilter) and returns
   * `destination`.
   * @param {Object} destination
   */
  connect(destination) {}

  /** Disables the filter slot; the node keeps it for a later connect(). */
  disconnect() {}

  /**
   * @param {Float32Array} frequencyHz
   * @param {Float32Array} magResponse
   * @param {Float32Array} phaseResponse
   */
  getFrequencyResponse(frequencyHz, magResponse, phaseResponse) {}

}

class AnalyserNode {

  /**
   * What the analyser taps: 0 = engine output (default), 1 = microphone
   * ring (see `bro.mic` / createMediaStreamSource), 2 = both summed
   * (the mic part is dropped while the mic is muted). Any other value
   * reads as 0. Connecting a MediaStreamAudioSourceNode to the analyser
   * sets it to 1.
   * @type {number}
   */
  source;

  /**
   * @type {number}
   */
  fftSize;

  /**
   * @readonly
   * @type {number}
   */
  frequencyBinCount;

  /**
   * @type {number}
   */
  minDecibels;

  /**
   * @type {number}
   */
  maxDecibels;

  /**
   * @type {number}
   */
  smoothingTimeConstant;

  /**
   * @param {Float32Array} array
   */
  getFloatFrequencyData(array) {}

  /**
   * @param {Uint8Array} array
   */
  getByteFrequencyData(array) {}

  /**
   * @param {Float32Array} array
   */
  getFloatTimeDomainData(array) {}

  /**
   * @param {Uint8Array} array
   */
  getByteTimeDomainData(array) {}

  /**
   * @param {Object} destination
   */
  connect(destination) {}

  disconnect() {}

}

class MediaStream {

  /**
   * @readonly
   * @type {boolean}
   */
  active;

}

class MediaStreamAudioSourceNode {

  /**
   * Returns `destination`. When it is an AnalyserNode, the analyser's
   * `source` becomes 1 (microphone). Throws TypeError with no argument.
   * @param {Object} destination
   */
  connect(destination) {}

  disconnect() {}

}

class AudioDestinationNode {

  /**
   * @readonly
   * @type {number}
   */
  maxChannelCount;

}

class VoiceAllocator {

  /**
   * @param {number} note
   * @param {number} velocity
   * @returns {number}
   */
  noteOn(note, velocity) {}

  /**
   * @param {number} note
   */
  noteOff(note) {}

  allNotesOff() {}

  /**
   * @returns {number}
   */
  voiceCount() {}

}

class ModMatrix {

  /**
   * @param {string} source
   * @param {string} dest
   * @param {number} amount
   */
  setRouting(source, dest, amount) {}

  /**
   * @param {string} source
   * @param {string} dest
   * @returns {number}
   */
  getRouting(source, dest) {}

  clear() {}

}

class MidiInput {

  /**
   * @returns {Array<MidiPort>}
   */
  listPorts() {}

  /**
   * @param {number} index
   * @returns {boolean}
   */
  openPort(index) {}

  closePort() {}

  /**
   * @returns {Array<MidiRawEvent>}
   */
  pollEvents() {}

}

class Sequence {

  /**
   * @type {number}
   */
  tempo;

  /**
   * @type {number}
   */
  length;

  /**
   * @type {boolean}
   */
  loop;

  /**
   * @param {number} beat
   * @param {number} note
   * @param {number} velocity
   * @param {number} duration
   */
  addNote(beat, note, velocity, duration) {}

  clearNotes() {}

  /**
   * @returns {Array<SequenceNote>}
   */
  getNotes() {}

}

class AudioContext {

  constructor() {}

  /**
   * @readonly
   * @type {number}
   */
  currentTime;

  /**
   * @readonly
   * @type {number}
   */
  sampleRate;

  /**
   * @readonly
   * @type {string}
   */
  state;

  /**
   * @readonly
   * @type {AudioDestinationNode}
   */
  destination;

  /**
   * @returns {OscillatorNode}
   */
  createOscillator() {}

  /**
   * @returns {GainNode}
   */
  createGain() {}

  /**
   * Allocates one of the engine's master filter slots (there are 4).
   * Throws Error("No filter slots available") when they are all taken;
   * dropping a node releases its slot.
   * @returns {BiquadFilterNode}
   */
  createBiquadFilter() {}

  /**
   * @returns {AnalyserNode}
   */
  createAnalyser() {}

  /**
   * Throws TypeError("Expected MediaStream argument") for anything that
   * is not a MediaStream; returns undefined with no argument.
   * @param {MediaStream} stream
   * @returns {MediaStreamAudioSourceNode}
   */
  createMediaStreamSource(stream) {}

  /**
   * @param {number} maxVoices
   * @returns {VoiceAllocator}
   */
  createVoiceAllocator(maxVoices) {}

  /**
   * @returns {ModMatrix}
   */
  createModMatrix() {}

  /**
   * @returns {MidiInput}
   */
  createMidiInput() {}

  /**
   * Throws TypeError("Expected VoiceAllocator argument") for anything
   * that is not a VoiceAllocator; returns undefined with no argument.
   * @param {VoiceAllocator} allocator
   * @returns {Sequence}
   */
  createSequence(allocator) {}

  suspend() {}

  resume() {}

  close() {}

  startRecording() {}

  /**
   * @returns {Float32Array|null}
   */
  stopRecording() {}

  /**
   * Loads WAV/FLAC/MP3/Ogg into a clip. `path` resolves like fs.* (app
   * directory relative); the clip is resampled to the engine rate.
   * @param {string} path
   * @returns {number} clip id, or -1
   */
  createClipFromFile(path) {}

  /**
   * Same as createClipFromFile but decodes on a worker thread. Resolves
   * with the clip id; rejects with an Error whose message starts with
   * the resolved path ("<path>: cannot open or decode file ..."). Throws
   * TypeError synchronously when no path is given.
   * @param {string} path
   * @returns {Promise<number>}
   */
  createClipFromFileAsync(path) {}

  /**
   * @param {ArrayBuffer} buffer
   * @returns {AudioDecodedBuffer|null}
   */
  decodeAudioData(buffer) {}

  /**
   * `path` resolves like fs.*.
   * @param {string} path
   * @returns {AudioDecodedBuffer|null}
   */
  decodeAudioFile(path) {}

  /**
   * `path` resolves like fs.* (its directory must exist).
   * @param {string} path
   * @returns {boolean}
   */
  exportRecordingToWav(path) {}

  /**
   * Writes interleaved float samples as a 16-bit WAV. `path` resolves
   * like fs.*. Throws TypeError("Expected Float32Array as second
   * argument") for a non-typed-array `samples`; false when channels or
   * sampleRate is not positive or the file cannot be written.
   * @param {string} path
   * @param {Float32Array} samples
   * @param {number} channels
   * @param {number} sampleRate
   * @returns {boolean}
   */
  saveWav(path, samples, channels, sampleRate) {}

  /**
   * Creates a clip from interleaved float samples (or an AudioBuffer).
   * When `sampleRate` is given and differs from the engine rate the
   * samples are resampled, so the clip plays at the right pitch.
   * @param {Float32Array|AudioBuffer} samples
   * @param {number} [channels=1]
   * @param {number} [sampleRate=engine rate]
   * @returns {number}
   */
  createClip(samples, channels, sampleRate) {}

  /**
   * @param {number} id
   */
  deleteClip(id) {}

  /**
   * @param {number} id
   * @returns {number}
   */
  getClipSampleCount(id) {}

  /**
   * @param {number} id
   * @returns {number}
   */
  getClipChannels(id) {}

  /**
   * Min/max pairs for drawing: a Float32Array of `numBins * 2` values,
   * `[min0, max0, min1, max1, ...]`, zero-filled for an unknown clip.
   * Returns undefined when `numBins` is missing or outside 1..1024.
   * @param {number} id
   * @param {number} numBins
   * @returns {Float32Array|undefined}
   */
  getClipWaveform(id, numBins) {}

  /**
   * Starts a clip and returns a playback id. A numeric `when` (engine
   * seconds, from `currentTime`) queues the start on the audio clock so
   * streamed chunks join gaplessly; a `when` at or before now plays
   * immediately, as does the 3-argument form.
   * @param {number} id
   * @param {number} [gain=1]
   * @param {boolean} [loop=false]
   * @param {number} [when]
   * @returns {number}
   */
  playClip(id, gain, loop, when) {}

  /**
   * Same as `playClip(id, gain, loop, when)` with a required `when`.
   * @param {number} id
   * @param {number} when
   * @param {number} [gain=1]
   * @param {boolean} [loop=false]
   * @returns {number}
   */
  playClipAt(id, when, gain, loop) {}

  /**
   * @param {number} id
   * @param {number} sendBusId
   * @param {number} amount
   */
  setPlaybackSend(id, sendBusId, amount) {}

  /**
   * Creates a push stream at the engine rate. `ringFrames` is the ring
   * capacity in frames; 0 (the default) means two seconds.
   * @param {number} [channels=1]
   * @param {number} [ringFrames=0]
   * @returns {number}
   */
  createStream(channels, ringFrames) {}

  /**
   * Pushes interleaved frames. Returns the number of frames written
   * (less than pushed when the ring is full). Throws TypeError("Expected
   * Float32Array samples") for a non-typed-array.
   * @param {number} id
   * @param {Float32Array} samples
   * @returns {number}
   */
  pushStreamSamples(id, samples) {}

  /**
   * @param {number} id
   */
  closeStream(id) {}

  /**
   * Streams a file from disk (decoded on a worker). `path` resolves like
   * fs.*. Options: `ringFrames`, `prebufferFrames` (frames decoded before
   * playback starts), `loop`, `gain`. Throws TypeError without a path
   * and Error when the file cannot be opened.
   * @param {string} path
   * @param {StreamFromFileOptions} [opts]
   * @returns {number}
   */
  createStreamFromFile(path, opts) {}

  /**
   * @param {number} id
   * @returns {StreamStats|null}
   */
  getStreamStats(id) {}

  /**
   * @param {number} id
   */
  stopPlayback(id) {}

  /**
   * @param {number} id
   * @param {number} gain
   */
  setPlaybackGain(id, gain) {}

  /**
   * @param {number} id
   * @param {boolean} loop
   */
  setPlaybackLoop(id, loop) {}

  /**
   * @param {number} id
   * @param {boolean} playing
   */
  setPlaybackPlaying(id, playing) {}

  /**
   * @param {number} id
   * @param {number} startFrame
   * @param {number} endFrame
   */
  setPlaybackRegion(id, startFrame, endFrame) {}

  /**
   * @param {number} id
   * @param {number} rate
   */
  setPlaybackRate(id, rate) {}

  /**
   * @param {number} id
   * @param {number} pan
   */
  setPlaybackPan(id, pan) {}

  /**
   * @param {number} id
   * @returns {number}
   */
  getPlaybackPosition(id) {}

  /**
   * @param {number} id
   * @returns {number}
   */
  getPlaybackPositionSeconds(id) {}

  /**
   * @param {number} id
   * @param {number} seconds
   */
  seekPlayback(id, seconds) {}

  // ── Offline rendering and analysis ──────────────────────────────────────

  /**
   * Renders `numFrames` through the whole pipeline without a device
   * (headless use) and returns the latest mono mixdown. With no `out`, a
   * fresh Float32Array of min(numFrames, analysis ring) frames; with a
   * typed-array `out`, that array filled in place (up to its length) and
   * returned. Undefined for a missing or non-positive `numFrames`.
   * @param {number} numFrames
   * @param {Float32Array} [out]
   * @returns {Float32Array|undefined}
   */
  renderBlock(numFrames, out) {}

  /**
   * Magnitude spectrum of the latest output, `numBins` values (1..8192;
   * undefined outside that range).
   * @param {number} numBins
   * @returns {Float32Array|undefined}
   */
  getSpectrum(numBins) {}

  // ── Wavetables ──────────────────────────────────────────────────────────

  /**
   * Builds a band-limited wavetable bank at the engine sample rate.
   * @param {string} type "saw" | "square" | "triangle"
   * @returns {number|undefined} bank id, or undefined for any other type
   */
  createWavetable(type) {}

  /**
   * Builds a bank from one cycle of a waveform (the whole array is the
   * cycle) at the engine sample rate. Throws TypeError("Expected
   * Float32Array") for a non-typed-array.
   * @param {Float32Array} waveform
   * @returns {number}
   */
  createWavetableFromWaveform(waveform) {}

  /**
   * @param {number} bankId
   */
  deleteWavetable(bankId) {}

  /**
   * Assigns a bank to a voice and switches the voice to "wavetable" in
   * one call (a bank on a voice still set to "sine" would be inaudible).
   * @param {number} voiceId
   * @param {number} bankId
   */
  setVoiceWavetable(voiceId, bankId) {}

  // ── Bus effect order and master effect shortcuts ────────────────────────

  /**
   * Reorders a bus's effect chain. `order` lists slot names ("filter",
   * "delay", "compressor", "reverb", "chorus", "distortion", "eq") in
   * processing order; an unknown name keeps that position's default
   * slot. Arrays that are empty or longer than the 7 slots are ignored.
   * @param {number} busId
   * @param {Array<string>} order
   */
  setBusEffectOrder(busId, order) {}

  /**
   * @param {number} busId
   * @param {number} seconds
   */
  setBusChorusBaseDelay(busId, seconds) {}

  /**
   * @param {number} busId
   * @returns {number}
   */
  getBusChorusBaseDelay(busId) {}

  /** Master-bus chorus (bus 0) shortcuts. */
  /** @param {boolean} enabled */
  setChorusEnabled(enabled) {}
  /** @param {number} hz */
  setChorusRate(hz) {}
  /** @param {number} depth */
  setChorusDepth(depth) {}
  /** @param {number} mix */
  setChorusMix(mix) {}
  /** @param {number} feedback */
  setChorusFeedback(feedback) {}
  /** @param {number} seconds */
  setChorusBaseDelay(seconds) {}

  /** Master-bus compressor (bus 0) shortcuts. */
  /** @param {boolean} enabled */
  setCompressorEnabled(enabled) {}
  /** @param {number} dB */
  setCompressorThreshold(dB) {}
  /** @param {number} ratio */
  setCompressorRatio(ratio) {}
  /** @param {number} seconds */
  setCompressorAttack(seconds) {}
  /** @param {number} seconds */
  setCompressorRelease(seconds) {}

  // ── Presets ─────────────────────────────────────────────────────────────
  // Presets are plain objects on the JS side and JSON strings on disk.
  // The *ToJson functions take an object and return the JSON string
  // (null with no argument); the *FromJson functions parse a JSON string
  // back into an object (null with no argument). The apply* functions
  // take objects. savePreset / loadPreset move JSON strings to and from
  // files through the fs.* path resolver.

  /**
   * @param {VoicePreset} preset
   * @returns {string|null}
   */
  voicePresetToJson(preset) {}

  /**
   * @param {string} json
   * @returns {VoicePreset|null}
   */
  voicePresetFromJson(json) {}

  /**
   * @param {BusPreset} preset
   * @returns {string|null}
   */
  busPresetToJson(preset) {}

  /**
   * @param {string} json
   * @returns {BusPreset|null}
   */
  busPresetFromJson(json) {}

  /**
   * @param {ModPreset} preset
   * @returns {string|null}
   */
  modPresetToJson(preset) {}

  /**
   * @param {string} json
   * @returns {ModPreset|null}
   */
  modPresetFromJson(json) {}

  /**
   * @param {EnginePreset} preset
   * @returns {string|null}
   */
  enginePresetToJson(preset) {}

  /**
   * @param {string} json
   * @returns {EnginePreset|null}
   */
  enginePresetFromJson(json) {}

  /**
   * @param {number} voiceId
   * @param {VoicePreset} preset
   */
  applyVoicePreset(voiceId, preset) {}

  /**
   * @param {number} busId
   * @param {BusPreset} preset
   */
  applyBusPreset(busId, preset) {}

  /**
   * @param {ModPreset} preset
   */
  applyModPreset(preset) {}

  /**
   * @param {EnginePreset} preset
   */
  applyEnginePreset(preset) {}

  /**
   * Writes a preset's JSON string to `path` (resolved like fs.*).
   * @param {string} json
   * @param {string} path
   * @returns {boolean}
   */
  savePreset(json, path) {}

  /**
   * Reads a preset's JSON string from `path` (resolved like fs.*).
   * @param {string} path
   * @returns {string|null} null when the file is missing or empty
   */
  loadPreset(path) {}

}

