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
 * @property {number} [beat]
 * @property {number} [note]
 * @property {number} [velocity]
 * @property {number} [duration]
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
   * @param {number} [when=0]
   */
  start(when) {}

  /**
   * @param {number} [when=0]
   */
  stop(when) {}

  /**
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
   * @param {Object} destination
   */
  connect(destination) {}

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
   * @returns {BiquadFilterNode}
   */
  createBiquadFilter() {}

  /**
   * @returns {AnalyserNode}
   */
  createAnalyser() {}

  /**
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
   * @returns {Sequence}
   */
  createSequence() {}

  suspend() {}

  resume() {}

  close() {}

  startRecording() {}

  /**
   * @returns {Float32Array|null}
   */
  stopRecording() {}

  /**
   * @param {string} path
   * @returns {number}
   */
  createClipFromFile(path) {}

  /**
   * @param {string} path
   * @returns {AsyncHandle}
   */
  createClipFromFileAsync(path) {}

  /**
   * @param {ArrayBuffer} buffer
   * @returns {AudioDecodedBuffer|null}
   */
  decodeAudioData(buffer) {}

  /**
   * @param {string} path
   * @returns {AudioDecodedBuffer|null}
   */
  decodeAudioFile(path) {}

  /**
   * @param {string} path
   * @returns {boolean}
   */
  exportRecordingToWav(path) {}

  /**
   * @param {string} path
   * @param {Float32Array} samples
   * @param {number} channels
   * @param {number} sampleRate
   * @returns {boolean}
   */
  saveWav(path, samples, channels, sampleRate) {}

  /**
   * @param {Float32Array} samples
   * @param {number} [channels=1]
   * @returns {number}
   */
  createClip(samples, channels) {}

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
   * @param {number} id
   * @param {number} points
   * @returns {Float32Array|null}
   */
  getClipWaveform(id, points) {}

  /**
   * @param {number} id
   * @param {number} [gain=1]
   * @param {boolean} [loop=false]
   * @param {number} [pan=0]
   * @returns {number}
   */
  playClip(id, gain, loop, pan) {}

  /**
   * @param {number} channels
   * @param {number} sampleRate
   * @returns {number}
   */
  createStream(channels, sampleRate) {}

  /**
   * @param {number} id
   * @param {Float32Array} samples
   * @returns {boolean}
   */
  pushStreamSamples(id, samples) {}

  /**
   * @param {number} id
   */
  closeStream(id) {}

  /**
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

}

