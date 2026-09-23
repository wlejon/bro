/**
 * =============================================================================
 * AudioContext engine API — broaudio's id-addressed voices, clips, buses
 * =============================================================================
 *
 * The second half of the audio surface. `audio-api.js` covers the
 * Web-Audio-shaped half (AudioContext's properties, lifecycle and node
 * factories, the node classes, getUserMedia) and the rules for how that layer
 * maps onto broaudio — read its header first. This file covers what the Web
 * Audio layer cannot express: the synth helpers (VoiceAllocator, ModMatrix,
 * MidiInput, Sequence) and the AudioContext methods that address broaudio's
 * engine directly by integer id — clips and their playbacks, streams, raw
 * voices, wavetables, buses and their effect chains, the master bus, the
 * listener and head model, offline rendering, and presets.
 *
 * Every AudioContext drives the same process-wide engine, so ids made through
 * one context are valid through any other.
 *
 * As in `audio-api.js`, underscore-prefixed properties on these objects
 * (`_laneCbs`, `_voiceSetup`, `_cc_<n>`, `_rawCb`, `_pitchBendCb`,
 * `_connectedAllocator`, `_allocator`, ...) are the binding's internal
 * bookkeeping, not API. Typed-array parameters are checked the same way: a
 * Float32Array parameter throws TypeError for another element type or a
 * detached array and never reinterprets its bytes.
 *
 * File paths (`createClipFromFile*`, `createStreamFromFile`, `savePreset`,
 * `loadPreset`) resolve the way `fs.*` does: relative to the app directory,
 * mount paths honoured. A path being written resolves through its parent
 * directory, which must exist.
 *
 * @example
 *   // A looping clip on its own bus with reverb, moving in 3D
 *   const ctx = new AudioContext();
 *   const clip = await ctx.createClipFromFileAsync('assets/loop.ogg');
 *   const bus = ctx.createBus();
 *   ctx.setBusReverbEnabled(bus, true);
 *   ctx.setBusReverbMix(bus, 0.4);
 *   const pb = ctx.playClip(clip, 0.8, true);
 *   ctx.setPlaybackBus(pb, bus);
 *   ctx.setPlaybackSpatialEnabled(pb, true);
 *   ctx.setPlaybackSpatialPosition(pb, 3, 0, -2);
 *
 * @example
 *   // Polyphonic synth from a MIDI keyboard
 *   const ctx = new AudioContext();
 *   const alloc = ctx.createVoiceAllocator(8);
 *   alloc.setVoiceSetup((voiceId, note, velocity) => {
 *     ctx.setVoiceFrequency(voiceId, 440 * 2 ** ((note - 69) / 12));  // noteOn does not
 *     ctx.setVoiceWaveform(voiceId, 'square');
 *     ctx.setVoiceRelease(voiceId, 0.3);
 *   });
 *   const midi = ctx.createMidiInput();
 *   if (midi.availablePorts().length) midi.open(0);
 *   midi.connectToAllocator(alloc);
 *   requestAnimationFrame(function tick() { midi.processEvents(); requestAnimationFrame(tick); });
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * Ring statistics for a streaming playback (`createStream` or
 * `createStreamFromFile`).
 * @typedef {Object} StreamStats
 * @property {number} decodedFrames -  Frames ever pushed into the ring.
 * @property {number} playedFrames -  Frames consumed by the mixer.
 * @property {number} bufferedFrames -  decodedFrames - playedFrames.
 * @property {number} underrunFrames -  Silent frames emitted while the ring was starved.
 * @property {boolean} finished -  Disk stream: end of file reached and ring drained.
 */

/**
 * Options for `createStreamFromFile`. Frame counts are at the engine rate.
 * Values are coerced like JS numbers / booleans.
 * @typedef {Object} StreamFromFileOptions
 * @property {number} [ringFrames=0] -  Ring capacity; 0 = about 2 s.
 * @property {number} [prebufferFrames=0] -  Frames decoded before playback starts; 0 = about 500 ms.
 * @property {boolean} [loop=false] -  Rewind the decoder at end of file (seamless; also follows setPlaybackLoop).
 * @property {number} [gain=1]
 */

/**
 * One note of a Sequence, as `Sequence.note(i)` returns it.
 * @typedef {Object} SequenceNote
 * @property {number} beat -  Beat position (0-based, fractional).
 * @property {number} beatPosition -  Same value as `beat`.
 * @property {number} note -  MIDI note number.
 * @property {number} velocity -  0..1.
 * @property {number} duration -  Length in beats.
 */

/**
 * One automation point, as `Sequence.automationPoint(lane, i)` returns it.
 * @typedef {Object} SequenceAutomationPoint
 * @property {number} beat
 * @property {number} value
 */

/**
 * A MIDI input port, from `MidiInput.availablePorts()`.
 * @typedef {Object} MidiPort
 * @property {number} index -  Pass to `MidiInput.open`.
 * @property {string} name
 */

/**
 * A MIDI message, as `MidiInput.onRawEvent` delivers it.
 * @typedef {Object} MidiRawEvent
 * @property {string} type -  'noteon' | 'noteoff' | 'controlchange' | 'pitchbend' |
 *   'programchange' | 'aftertouch' | 'channelpressure'.
 * @property {number} channel -  0..15.
 * @property {number} data1 -  Note number or CC number.
 * @property {number} data2 -  Velocity or CC value (0..127).
 * @property {number} pitchBend -  -8192..8191 (pitch-bend messages).
 * @property {number} timestamp -  Engine time in seconds.
 */

/**
 * Plain-object form of a broaudio VoicePreset: what `voicePresetToJson` and
 * `applyVoicePreset` take and `voicePresetFromJson` gives back. Every field
 * is optional on input; missing ones keep the default shown.
 * @typedef {Object} VoicePreset
 * @property {string} [waveform='sine'] -  Any waveform name (see OscillatorNode.type); a
 *   wavetable voice reads back as 'custom'.
 * @property {number} [frequency=440]
 * @property {number} [gain=1]
 * @property {number} [pan=0]
 * @property {number} [pitchBend=0] -  Semitones.
 * @property {number} [attackTime=0.01] -  Seconds.
 * @property {number} [decayTime=0.1] -  Seconds.
 * @property {number} [sustainLevel=1] -  0..1.
 * @property {number} [releaseTime=0.04] -  Seconds.
 * @property {boolean} [filterEnabled=false]
 * @property {string} [filterType='lowpass']
 * @property {number} [filterFreq=1000]
 * @property {number} [filterQ=1]
 * @property {number} [unisonCount=1] -  1..8.
 * @property {number} [unisonDetune=0.15] -  Semitones.
 * @property {number} [unisonStereoWidth=0.7] -  0..1.
 */

/**
 * One bus filter slot inside a BusPreset.
 * @typedef {Object} FilterPreset
 * @property {boolean} [enabled=false]
 * @property {string} [type='lowpass']
 * @property {number} [frequency=1000]
 * @property {number} [Q=1]
 * @property {number} [gainDB=0]
 */

/**
 * Plain-object form of a BusPreset. Sub-objects and their defaults:
 * `delay` { enabled=false, time=0.3 s, feedback=0.3, mix=0.5 };
 * `compressor` { enabled=false, threshold=0.7 (linear 0..1), ratio=4, attackMs=1, releaseMs=100 };
 * `reverb` { enabled=false, roomSize=0.85, damping=0.5, mix=0.3 };
 * `chorus` { enabled=false, rate=0.5 Hz, depth=0.005 s, mix=0.5, feedback=0, baseDelay=0.01 s };
 * `distortion` { enabled=false, mode='softclip', drive=1, mix=1, outputGain=1, crushBits=16, crushRate=1 };
 * `eq` { enabled=false, bandGains=[7 x 0 dB], masterGain=0 }.
 * @typedef {Object} BusPreset
 * @property {number} [gain=1]
 * @property {number} [pan=0]
 * @property {Array<FilterPreset>} [filters] -  Up to 4 slots.
 * @property {Object} [delay]
 * @property {Object} [compressor]
 * @property {Object} [reverb]
 * @property {Object} [chorus]
 * @property {Object} [distortion]
 * @property {Object} [eq]
 * @property {Array<string>} [effectOrder] -  Slot names, as setBusEffectOrder takes them;
 *   read back with 'equalizer' for the EQ slot.
 */

/**
 * Plain-object form of a ModPreset.
 * `lfos`: up to 4 of { shape='sine', rate=1, depth=1, offset=0, bipolar=true, sync=false }.
 * `routes`: any number of { source='lfo1', dest='pitch', amount=0, enabled=true }.
 * Names are the ModMatrix names.
 * @typedef {Object} ModPreset
 * @property {Array<Object>} [lfos]
 * @property {Array<Object>} [routes]
 */

/**
 * Plain-object form of an EnginePreset.
 * @typedef {Object} EnginePreset
 * @property {number} [masterGain=0.5]
 * @property {{enabled: boolean, thresholdDb: number, releaseMs: number}} [limiter] -
 *   Defaults { enabled: true, thresholdDb: -6, releaseMs: 50 }.
 * @property {BusPreset} [masterBus]
 * @property {Array<BusPreset>} [buses] -  Child buses (index 0 is the first child, not master).
 * @property {ModPreset} [modulation]
 */

// ── Synth helpers ────────────────────────────────────────────────────────────

/**
 * A fixed pool of engine voices with MIDI-style note tracking and voice
 * stealing. `new VoiceAllocator(maxVoices)` or `ctx.createVoiceAllocator`.
 * Voices come out with engine defaults; configure them in `setVoiceSetup`.
 * The allocator does NOT turn the note number into a pitch (it only feeds
 * the ModMatrix 'keytracking' source): set the frequency in the voice-setup
 * callback, or every note sounds at the voice's current frequency.
 */
class VoiceAllocator {

  /** @param {number} [maxVoices=16]  <= 0 means 16. */
  constructor(maxVoices) {}

  /**
   * Allocates (or steals) a voice, runs the voice-setup callback, sets the
   * voice gain to `velocity` (after the callback, so it wins), and starts it.
   * Needs at least `note` and `velocity`.
   * @param {number} note  MIDI note number.
   * @param {number} velocity  0..1.
   * @param {number} [when=currentTime]  Engine seconds; 0 also means now.
   * @returns {number} voice id, or -1 when the note was dropped
   */
  noteOn(note, velocity, when) {}

  /**
   * Releases every voice playing `note`.
   * @param {number} note
   * @param {number} [when=currentTime]
   */
  noteOff(note, when) {}

  /** @param {number} [when=currentTime] */
  allNotesOff(when) {}

  /**
   * 'oldest' (default: earliest-started voice) | 'quietest' (lowest
   * envelope) | 'samenote' (a voice on the same note, else oldest) | 'none'
   * (drop the new note). Unknown names mean 'oldest'.
   * @param {string} policy
   */
  setStealPolicy(policy) {}

  /** @param {number} count */
  setMaxVoices(count) {}

  /**
   * Called synchronously for every voice this allocator hands out (from
   * `noteOn`, a Sequence's `update()`, or `MidiInput.processEvents()`),
   * after allocation and before the note starts. Pass a non-function to
   * clear it.
   * @param {function(number, number, number): void|null} fn  (voiceId, note, velocity)
   */
  setVoiceSetup(fn) {}

  /**
   * @param {number} note
   * @returns {number} the first voice id sounding `note`, or -1
   */
  voiceForNote(note) {}

  /** Voices currently sounding (not yet released). @readonly @type {number} */
  activeVoiceCount;

}

/**
 * The engine's modulation matrix: 4 LFOs plus up to 64 routes from sources
 * to voice destinations, applied to every voice. There is exactly one;
 * `new ModMatrix()`, `ctx.createModMatrix()` and `ctx.getModMatrix()` all
 * return a handle to it.
 */
class ModMatrix {

  /**
   * LFO index 0..3; other indices are ignored (true of all setLfo* methods).
   * @param {number} index
   * @param {string} shape  'sine' | 'triangle' | 'square' | 'sawup' | 'sawdown' |
   *   'sampleandhold'; unknown names mean 'sine'.
   */
  setLfoShape(index, shape) {}

  /** @param {number} index @param {number} hz  Default 1. */
  setLfoRate(index, hz) {}

  /** @param {number} index @param {number} depth  Output scale 0..1, default 1. */
  setLfoDepth(index, depth) {}

  /** @param {number} index @param {number} offset  DC offset -1..1, default 0. */
  setLfoOffset(index, offset) {}

  /** @param {number} index @param {boolean} bipolar  true (default): -1..1; false: 0..1. */
  setLfoBipolar(index, bipolar) {}

  /** @param {number} index @param {boolean} sync  Restart the LFO phase on note-on. Default false. */
  setLfoSync(index, sync) {}

  /**
   * Needs all three arguments.
   * @param {string} source  'lfo1' | 'lfo2' | 'lfo3' | 'lfo4' | 'envelope' | 'velocity' |
   *   'keytracking' (note / 127) | 'modwheel' | 'aftertouch'; unknown names mean 'lfo1'.
   * @param {string} dest  'pitch' (semitones) | 'gain' | 'pan' | 'filterfreq' | 'filterq' |
   *   'pulsewidth' | 'delaysend'; unknown names mean 'pitch'.
   * @param {number} amount  Scale applied to the source.
   * @returns {number} route id, or -1 (bad arguments or 64 routes already)
   */
  addRoute(source, dest, amount) {}

  /** @param {number} routeId */
  removeRoute(routeId) {}

  /** @param {number} routeId @param {number} amount */
  setRouteAmount(routeId, amount) {}

  /** @param {number} routeId @param {boolean} enabled */
  setRouteEnabled(routeId, enabled) {}

  clearAllRoutes() {}

  /** Value of the 'modwheel' source. @param {number} value  0..1 */
  setModWheel(value) {}

  /** Value of the 'aftertouch' source. @param {number} value  0..1 */
  setAftertouch(value) {}

  /** @readonly @type {number} */
  routeCount;

}

/**
 * A MIDI input port. Messages queue up on a background thread; nothing is
 * delivered until `processEvents()` is called, which runs the callbacks and
 * the allocator routing on the calling thread. Call it once per frame.
 */
class MidiInput {

  /** @returns {Array<MidiPort>} */
  availablePorts() {}

  /**
   * @param {number} portIndex
   * @returns {boolean} whether the port opened
   */
  open(portIndex) {}

  close() {}

  /** @readonly @type {boolean} */
  isOpen;

  /**
   * Note-on / note-off messages drive this allocator's noteOn / noteOff
   * (with its voice-setup callback) during processEvents().
   * @param {VoiceAllocator} allocator
   */
  connectToAllocator(allocator) {}

  /**
   * Registers (or, with a non-function, clears) the handler for one CC number.
   * Numbers outside 0..127 are ignored.
   * @param {number} cc
   * @param {function(number, number, number): void|null} fn  (channel, cc, value)
   */
  onControlChange(cc, fn) {}

  /** @param {function(number, number): void|null} fn  (channel, value -8192..8191) */
  onPitchBend(fn) {}

  /** Receives every message before dispatch. @param {function(MidiRawEvent): void|null} fn */
  onRawEvent(fn) {}

  /**
   * Feeds one raw MIDI message in as if it had arrived from the port: the
   * same parse and the same queue, dispatched (raw callback, CC and
   * pitch-bend handlers, allocator routing) by the next `processEvents()`.
   * Works with no port open, so it serves tests, headless runs and on-screen
   * keyboards. Do not call it while an open port is delivering messages.
   *
   * Understood: note on (velocity 0 is a note off), note off, control change,
   * pitch bend, program change, polyphonic aftertouch, channel pressure.
   * `bytes` is a plain array of numbers or a typed array: a 1-byte typed
   * array (Uint8Array, Int8Array, Uint8ClampedArray) is taken as raw bytes,
   * and any other array is read element-wise, each value masked to 0..255.
   * TypeError for a typed array whose buffer is detached.
   * @param {Array<number>|Uint8Array} bytes  Status byte, then data bytes.
   * @param {number} [timestamp]  Engine seconds, reported as the event's
   *   `timestamp`. Missing or negative: `currentTime`.
   * @returns {boolean} false for a system message (sysex, clock, ...), a
   *   truncated message, a non-array argument, or a full queue (1024
   *   pending events; the message is dropped)
   */
  injectMessage(bytes, timestamp) {}

  /** Drains the queue and dispatches it. */
  processEvents() {}

}

/**
 * A beat-based note sequence played through a VoiceAllocator.
 * `new Sequence(allocator)` (null when the argument is not an allocator) or
 * `ctx.createSequence(allocator)`. Nothing plays unless `update()` is called
 * regularly (each frame): it fires the notes and automation due by then.
 * Beats are quarter notes.
 */
class Sequence {

  /** @param {VoiceAllocator} allocator */
  constructor(allocator) {}

  /**
   * @param {number} bpm  Clamped to 1..999.
   * @param {number} [engineTime]  When given while playing, the tempo change
   *   keeps the beat position continuous at that engine time.
   */
  setBPM(bpm, engineTime) {}

  /** Default 120. @readonly @type {number} */
  bpm;

  /** Informational only. Needs both arguments. @param {number} numerator @param {number} denominator */
  setTimeSignature(numerator, denominator) {}

  /**
   * Needs all four arguments.
   * @param {number} beat  Beat position (0-based, fractional).
   * @param {number} note  MIDI note.
   * @param {number} velocity  0..1.
   * @param {number} duration  Beats.
   */
  addNote(beat, note, velocity, duration) {}

  /** @param {number} index  Position in beat order. */
  removeNote(index) {}

  clearNotes() {}

  /** @readonly @type {number} */
  noteCount;

  /**
   * @param {number} index
   * @returns {SequenceNote|null}
   */
  note(index) {}

  /** @param {number} [when=currentTime] */
  play(when) {}

  stop() {}

  /** @param {number} [when=currentTime] */
  pause(when) {}

  /** @param {number} [when=currentTime] */
  resume(when) {}

  /** @readonly @type {boolean} */
  playing;

  /** @readonly @type {boolean} */
  paused;

  /** @param {boolean} enabled */
  setLoopEnabled(enabled) {}

  /** @readonly @type {boolean} */
  loopEnabled;

  /**
   * Needs both arguments.
   * @param {number} startBeat
   * @param {number} endBeat  0 = end of the sequence.
   */
  setLoopRange(startBeat, endBeat) {}

  /**
   * @param {number} [when=currentTime]
   * @returns {number}
   */
  currentBeat(when) {}

  /**
   * Fires the notes and automation values due by `when`.
   * @param {number} [when=currentTime]
   */
  update(when) {}

  /**
   * Adds an automation lane whose value is handed to `fn` during update().
   * @param {function(number): void} fn
   * @returns {number} lane index, or -1 when `fn` is not a function
   */
  addAutomationLane(fn) {}

  /** Later lanes shift down one index. @param {number} lane */
  removeAutomationLane(lane) {}

  clearAutomationLanes() {}

  /** @readonly @type {number} */
  automationLaneCount;

  /**
   * Out-of-range lanes are ignored by every per-lane method.
   * @param {number} lane @param {number} beat @param {number} value
   */
  addAutomationPoint(lane, beat, value) {}

  /** @param {number} lane @param {number} pointIndex */
  removeAutomationPoint(lane, pointIndex) {}

  /** @param {number} lane */
  clearAutomationPoints(lane) {}

  /** @param {number} lane @param {string} mode  'linear' (default) | 'step' | 'smooth'. */
  setAutomationInterpMode(lane, mode) {}

  /** @param {number} lane @returns {string} */
  automationInterpMode(lane) {}

  /** @param {number} lane @returns {number} */
  automationPointCount(lane) {}

  /**
   * @param {number} lane @param {number} pointIndex
   * @returns {SequenceAutomationPoint|null}
   */
  automationPoint(lane, pointIndex) {}

}

/**
 * AudioContext, continued: the engine methods. The constructor, properties,
 * lifecycle, node factories, decoding and recording are in `audio-api.js`.
 */
class AudioContext {

  // ── Clips ───────────────────────────────────────────────────────────────
  // A clip is decoded PCM held by the engine (id); each playClip call makes
  // a playback instance (another id) that the setPlayback* methods address.

  /**
   * From an AudioBuffer (getChannelData writes included), resampled from the
   * buffer's `sampleRate` to the engine rate (`channels` and `sampleRate`
   * are then ignored). Or from interleaved float samples in a Float32Array
   * or an ArrayBuffer (its bytes read as float32); with a numeric
   * `sampleRate` that differs from the engine rate these are resampled.
   * TypeError for any other argument, another typed array (an Int16Array is
   * not converted), a detached array, or empty data.
   * @param {AudioBuffer|Float32Array|ArrayBuffer} samples
   * @param {number} [channels=1]
   * @param {number} [sampleRate]  Raw samples only. Default: the engine rate.
   * @returns {number} clip id (-1 with no argument or an empty AudioBuffer)
   */
  createClip(samples, channels, sampleRate) {}

  /**
   * Loads WAV / FLAC / MP3 / Ogg synchronously, resampled to the engine rate.
   * @param {string} path
   * @returns {number} clip id, or -1
   */
  createClipFromFile(path) {}

  /**
   * Decodes on a worker thread. Resolves with the clip id; rejects with an
   * Error whose message starts with the resolved path ("<path>: <reason>").
   * TypeError, thrown synchronously, without a path. The promise settles on
   * a later frame (the host ticks it once per frame).
   * @param {string} path
   * @returns {Promise<number>}
   */
  createClipFromFileAsync(path) {}

  /** @param {number} clipId */
  deleteClip(clipId) {}

  /** @param {number} clipId @returns {number} frames */
  getClipSampleCount(clipId) {}

  /** @param {number} clipId @returns {number} */
  getClipChannels(clipId) {}

  /**
   * Min/max pairs for drawing: `numBins * 2` values `[min0, max0, min1,
   * max1, ...]`, zero-filled for an unknown clip.
   * @param {number} clipId
   * @param {number} numBins  1..1024.
   * @returns {Float32Array|undefined} undefined for numBins outside 1..1024
   */
  getClipWaveform(clipId, numBins) {}

  /**
   * Starts a clip. A numeric `when` (engine seconds) queues the start on the
   * audio clock so consecutive chunks join gaplessly; a `when` at or before
   * now plays immediately, as does leaving it out.
   * @param {number} clipId
   * @param {number} [gain=1]
   * @param {boolean} [loop=false]
   * @param {number} [when]
   * @returns {number} playback id (-1 on failure)
   */
  playClip(clipId, gain, loop, when) {}

  /**
   * `playClip` with `when` second. Needs at least clipId and when.
   * @param {number} clipId
   * @param {number} when
   * @param {number} [gain=1]
   * @param {boolean} [loop=false]
   * @returns {number} playback id
   */
  playClipAt(clipId, when, gain, loop) {}

  /** Same as stopPlayback: takes a PLAYBACK id, not a clip id. @param {number} playbackId */
  stopClip(playbackId) {}

  /**
   * True while the playback is producing audio, answered from its own state:
   * started, its `when` reached, not paused, not finished, not stopped. So a
   * playback at position 0 that has begun counts, and one scheduled for
   * later, paused, finished (parked at its end) or stopped does not; neither
   * does an unknown id. A `createStream` stream counts for as long as it is
   * open and not paused, even while its ring is empty; a
   * `createStreamFromFile` stream starts counting once its prebuffer is
   * decoded.
   * @param {number} playbackId
   * @returns {boolean}
   */
  isClipPlaying(playbackId) {}

  /** Same as setPlaybackGain. @param {number} playbackId @param {number} gain */
  setClipGain(playbackId, gain) {}

  /** Same as setPlaybackPan. @param {number} playbackId @param {number} pan */
  setClipPan(playbackId, pan) {}

  /** Same as setPlaybackLoop. @param {number} playbackId @param {boolean} loop */
  setClipLoop(playbackId, loop) {}

  // ── Playback instances (clips and streams) ──────────────────────────────

  /** @param {number} playbackId */
  stopPlayback(playbackId) {}

  /** @param {number} playbackId @param {number} gain */
  setPlaybackGain(playbackId, gain) {}

  /** @param {number} playbackId @param {number} pan  -1..1 */
  setPlaybackPan(playbackId, pan) {}

  /** @param {number} playbackId @param {boolean} loop */
  setPlaybackLoop(playbackId, loop) {}

  /** Pause (false) or resume (true) in place. @param {number} playbackId @param {boolean} playing */
  setPlaybackPlaying(playbackId, playing) {}

  /**
   * Plays only frames [startFrame, endFrame) (clamped to the clip) and
   * rewinds to the region start. Needs all three arguments.
   * @param {number} playbackId
   * @param {number} startFrame
   * @param {number} endFrame
   */
  setPlaybackRegion(playbackId, startFrame, endFrame) {}

  /** @param {number} playbackId @param {number} rate  Clamped to 0.01..16. */
  setPlaybackRate(playbackId, rate) {}

  /**
   * @param {number} playbackId
   * @returns {number} normalized position 0..1 within the region
   */
  getPlaybackPosition(playbackId) {}

  /**
   * Clips: seconds from the region start. Disk streams: file time. Live
   * streams: seconds consumed since opening. 0 for an unknown id.
   * @param {number} playbackId
   * @returns {number}
   */
  getPlaybackPositionSeconds(playbackId) {}

  /**
   * Clips: seconds from the region start, clamped to it. Disk streams: file
   * time (brief silence while the worker refills; restarts a finished
   * stream). Live streams: no-op.
   * @param {number} playbackId
   * @param {number} seconds
   */
  seekPlayback(playbackId, seconds) {}

  /** Route the playback to a bus (0 = master). @param {number} playbackId @param {number} busId */
  setPlaybackBus(playbackId, busId) {}

  /** Aux send, amount 0..1. @param {number} playbackId @param {number} sendBusId @param {number} amount */
  setPlaybackSend(playbackId, sendBusId, amount) {}

  /** @param {number} playbackId @param {boolean} enabled */
  setPlaybackSpatialEnabled(playbackId, enabled) {}

  /** Needs all four arguments. @param {number} playbackId @param {number} x @param {number} y @param {number} z */
  setPlaybackSpatialPosition(playbackId, x, y, z) {}

  /** For Doppler. Needs all four arguments. @param {number} playbackId @param {number} x @param {number} y @param {number} z */
  setPlaybackSpatialVelocity(playbackId, x, y, z) {}

  /** @param {number} playbackId @param {number} distance  Minimum 0.001. */
  setPlaybackSpatialRefDistance(playbackId, distance) {}

  /** @param {number} playbackId @param {number} distance  Minimum 0.001. */
  setPlaybackSpatialMaxDistance(playbackId, distance) {}

  /** @param {number} playbackId @param {number} rolloff  Minimum 0. */
  setPlaybackSpatialRolloff(playbackId, rolloff) {}

  /** @param {number} playbackId @param {string} model  'inverse' (default for unknown) | 'linear' | 'exponential'. */
  setPlaybackSpatialDistanceModel(playbackId, model) {}

  /** Low-pass muffling, 0..1. @param {number} playbackId @param {number} occlusion */
  setPlaybackSpatialOcclusion(playbackId, occlusion) {}

  /** Last Doppler ratio applied (1 until one was). @param {number} playbackId @returns {number} */
  getPlaybackDopplerRatio(playbackId) {}

  // ── Streams ─────────────────────────────────────────────────────────────

  /**
   * A live PCM source fed by pushStreamSamples. The id is a playback id, so
   * every setPlayback* method applies.
   * @param {number} [channels=1]
   * @param {number} [ringFrames=0]  0 = about 2 s at the engine rate.
   * @returns {number} playback id, or -1
   */
  createStream(channels, ringFrames) {}

  /**
   * Appends interleaved samples, which must be at the engine rate.
   * TypeError for anything but a Float32Array (another element type or a
   * detached array included).
   * @param {number} playbackId
   * @param {Float32Array} samples
   * @returns {number} frames written (fewer than given when the ring is full)
   */
  pushStreamSamples(playbackId, samples) {}

  /** Closes a live or disk stream. @param {number} playbackId */
  closeStream(playbackId) {}

  /**
   * Plays a file from disk, decoding incrementally on a worker (WAV, FLAC,
   * MP3, Ogg Vorbis; up to 2 channels). Playback starts once the prebuffer
   * is decoded. TypeError without a path; Error("createStreamFromFile: <reason>")
   * when the file cannot be opened.
   * @param {string} path
   * @param {StreamFromFileOptions} [options]
   * @returns {number} playback id
   */
  createStreamFromFile(path, options) {}

  /**
   * @param {number} playbackId
   * @returns {StreamStats|null} null when the id is not a stream
   */
  getStreamStats(playbackId) {}

  // ── Voices ──────────────────────────────────────────────────────────────
  // Raw engine voices (an OscillatorNode's voiceId is one too). Waveform
  // names are OscillatorNode.type's.

  /** A new stopped voice. @returns {number} voice id */
  createVoice() {}

  /** @param {number} voiceId */
  removeVoice(voiceId) {}

  /** @param {number} voiceId @param {number} [when=0]  Engine seconds; 0 / past = now. */
  startVoice(voiceId, when) {}

  /** Enters release. @param {number} voiceId @param {number} [when=0] */
  stopVoice(voiceId, when) {}

  /** Keep the voice (and its id) after its release finishes instead of letting the engine reap it. @param {number} voiceId @param {boolean} persistent */
  setVoicePersistent(voiceId, persistent) {}

  /**
   * Sets the note and velocity the ModMatrix sees ('keytracking',
   * 'velocity' sources) and restarts synced LFOs. It does NOT set the pitch;
   * use setVoiceFrequency for that.
   * @param {number} voiceId @param {number} note @param {number} [velocity=1]
   */
  setVoiceNote(voiceId, note, velocity) {}

  /** @param {number} voiceId @param {string} waveform */
  setVoiceWaveform(voiceId, waveform) {}

  /** @param {number} voiceId @param {number} hz */
  setVoiceFrequency(voiceId, hz) {}

  /** @param {number} voiceId @param {number} gain */
  setVoiceGain(voiceId, gain) {}

  /** @param {number} voiceId @param {number} pan  -1..1 */
  setVoicePan(voiceId, pan) {}

  /** @param {number} voiceId @param {number} semitones */
  setVoicePitchBend(voiceId, semitones) {}

  /** @param {number} voiceId @param {number} seconds */
  setVoiceAttackTime(voiceId, seconds) {}
  /** @param {number} voiceId @param {number} seconds */
  setVoiceDecayTime(voiceId, seconds) {}
  /** @param {number} voiceId @param {number} level  0..1 */
  setVoiceSustainLevel(voiceId, level) {}
  /** @param {number} voiceId @param {number} seconds */
  setVoiceReleaseTime(voiceId, seconds) {}

  /** Same as setVoiceAttackTime. @param {number} voiceId @param {number} seconds */
  setVoiceAttack(voiceId, seconds) {}
  /** Same as setVoiceDecayTime. @param {number} voiceId @param {number} seconds */
  setVoiceDecay(voiceId, seconds) {}
  /** Same as setVoiceSustainLevel. @param {number} voiceId @param {number} level */
  setVoiceSustain(voiceId, level) {}
  /** Same as setVoiceReleaseTime. @param {number} voiceId @param {number} seconds */
  setVoiceRelease(voiceId, seconds) {}

  /** Per-voice filter. @param {number} voiceId @param {boolean} enabled */
  setVoiceFilterEnabled(voiceId, enabled) {}
  /** @param {number} voiceId @param {string} type  BiquadFilterNode.type names. */
  setVoiceFilterType(voiceId, type) {}
  /** @param {number} voiceId @param {number} hz  Clamped to 20..20000. */
  setVoiceFilterFrequency(voiceId, hz) {}
  /** @param {number} voiceId @param {number} q  Clamped to 0.1..30. */
  setVoiceFilterQ(voiceId, q) {}

  /** @param {number} voiceId @param {number} count  1..8 */
  setVoiceUnisonCount(voiceId, count) {}
  /** @param {number} voiceId @param {number} semitones  0..2 */
  setVoiceUnisonDetune(voiceId, semitones) {}
  /** @param {number} voiceId @param {number} width  0..1 */
  setVoiceUnisonStereoWidth(voiceId, width) {}

  /**
   * Assigns a wavetable bank and switches the voice to 'wavetable' in one
   * call (an unknown bank id does nothing).
   * @param {number} voiceId
   * @param {number} bankId
   */
  setVoiceWavetable(voiceId, bankId) {}

  /** @param {number} voiceId @param {boolean} enabled */
  setVoiceSpatialEnabled(voiceId, enabled) {}
  /** Needs all four arguments. @param {number} voiceId @param {number} x @param {number} y @param {number} z */
  setVoiceSpatialPosition(voiceId, x, y, z) {}
  /** Needs all four arguments. @param {number} voiceId @param {number} x @param {number} y @param {number} z */
  setVoiceSpatialVelocity(voiceId, x, y, z) {}
  /** @param {number} voiceId @param {number} distance */
  setVoiceSpatialRefDistance(voiceId, distance) {}
  /** @param {number} voiceId @param {number} distance */
  setVoiceSpatialMaxDistance(voiceId, distance) {}
  /** @param {number} voiceId @param {number} rolloff */
  setVoiceSpatialRolloff(voiceId, rolloff) {}
  /** @param {number} voiceId @param {string} model  'inverse' | 'linear' | 'exponential' */
  setVoiceSpatialDistanceModel(voiceId, model) {}
  /** @param {number} voiceId @param {number} occlusion  0..1 */
  setVoiceSpatialOcclusion(voiceId, occlusion) {}
  /** @param {number} voiceId @returns {number} */
  getVoiceDopplerRatio(voiceId) {}

  /** @param {number} voiceId @param {number} busId */
  setVoiceBus(voiceId, busId) {}
  /** @param {number} voiceId @param {number} sendBusId @param {number} amount  0..1 */
  setVoiceSend(voiceId, sendBusId, amount) {}

  /** Sample-accurate start at engine time `when`. Needs both arguments. @param {number} voiceId @param {number} when */
  scheduleNoteOn(voiceId, when) {}
  /** Sample-accurate release at engine time `when`. Needs both arguments. @param {number} voiceId @param {number} when */
  scheduleNoteOff(voiceId, when) {}

  // ── Wavetables ──────────────────────────────────────────────────────────

  /**
   * A band-limited bank at the engine rate.
   * @param {string} type  'saw' | 'square' | 'triangle'
   * @returns {number|undefined} bank id, undefined for any other type
   */
  createWavetable(type) {}

  /**
   * A bank from one cycle (the whole array is the cycle), built at the
   * engine rate. TypeError for anything but a Float32Array (another element
   * type or a detached array included).
   * @param {Float32Array} waveform
   * @returns {number} bank id
   */
  createWavetableFromWaveform(waveform) {}

  /** Voices already using the bank keep it. @param {number} bankId */
  deleteWavetable(bankId) {}

  // ── Buses ───────────────────────────────────────────────────────────────
  // Bus 0 is the master; createBus makes a child that feeds master. Every
  // bus has the same effect chain. Getters return a neutral default for an
  // unknown bus. Setters need all their arguments.

  /** @returns {number} bus id */
  createBus() {}
  /** Master cannot be deleted. @param {number} busId */
  deleteBus(busId) {}
  /** @param {number} busId @param {number} gain  0..2 */
  setBusGain(busId, gain) {}
  /** @param {number} busId @returns {number} */
  getBusGain(busId) {}
  /** @param {number} busId @param {number} pan  -1..1 */
  setBusPan(busId, pan) {}
  /** @param {number} busId @returns {number} */
  getBusPan(busId) {}
  /** @param {number} busId @param {boolean} muted */
  setBusMuted(busId, muted) {}
  /** @param {number} busId @returns {boolean} */
  getBusMuted(busId) {}
  /**
   * While any bus is soloed, only soloed buses, their descendants and their
   * ancestors reach the mix. Mute still wins.
   * @param {number} busId @param {boolean} solo
   */
  setBusSolo(busId, solo) {}
  /** @param {number} busId @returns {boolean} */
  getBusSolo(busId) {}
  /** Aux send from one bus to another. @param {number} busId @param {number} sendBusId @param {number} amount  0..1 */
  setBusSend(busId, sendBusId, amount) {}
  /** @param {number} busId @returns {number} */
  getBusPeakL(busId) {}
  /** @param {number} busId @returns {number} */
  getBusPeakR(busId) {}
  /** @param {number} busId @returns {number} */
  getBusRmsL(busId) {}
  /** @param {number} busId @returns {number} */
  getBusRmsR(busId) {}

  /** One of the bus's 4 filter slots. @param {number} busId @returns {number} slot, or -1 */
  allocateBusFilterSlot(busId) {}
  /** @param {number} busId @param {number} slot */
  releaseBusFilterSlot(busId, slot) {}
  /** @param {number} busId @param {number} slot @param {boolean} enabled */
  setBusFilterEnabled(busId, slot, enabled) {}
  /** @param {number} busId @param {number} slot @returns {boolean} */
  getBusFilterEnabled(busId, slot) {}
  /** @param {number} busId @param {number} slot @param {string} type  BiquadFilterNode.type names. */
  setBusFilterType(busId, slot, type) {}
  /** @param {number} busId @param {number} slot @returns {string} */
  getBusFilterType(busId, slot) {}
  /** @param {number} busId @param {number} slot @param {number} hz  20..20000 */
  setBusFilterFrequency(busId, slot, hz) {}
  /** @param {number} busId @param {number} slot @returns {number} */
  getBusFilterFrequency(busId, slot) {}
  /** @param {number} busId @param {number} slot @param {number} q  0.1..30 */
  setBusFilterQ(busId, slot, q) {}
  /** @param {number} busId @param {number} slot @returns {number} */
  getBusFilterQ(busId, slot) {}
  /** @param {number} busId @param {number} slot @param {number} dB  -40..40 */
  setBusFilterGain(busId, slot, dB) {}
  /** @param {number} busId @param {number} slot @returns {number} */
  getBusFilterGain(busId, slot) {}

  /** @param {number} busId @param {boolean} enabled */
  setBusDelayEnabled(busId, enabled) {}
  /** @param {number} busId @returns {boolean} */
  getBusDelayEnabled(busId) {}
  /** @param {number} busId @param {number} seconds  0.001..2 */
  setBusDelayTime(busId, seconds) {}
  /** @param {number} busId @returns {number} */
  getBusDelayTime(busId) {}
  /** @param {number} busId @param {number} feedback  0..0.95 */
  setBusDelayFeedback(busId, feedback) {}
  /** @param {number} busId @returns {number} */
  getBusDelayFeedback(busId) {}
  /** @param {number} busId @param {number} mix  0..1 */
  setBusDelayMix(busId, mix) {}
  /** @param {number} busId @returns {number} */
  getBusDelayMix(busId) {}

  /** @param {number} busId @param {boolean} enabled */
  setBusReverbEnabled(busId, enabled) {}
  /** @param {number} busId @returns {boolean} */
  getBusReverbEnabled(busId) {}
  /** @param {number} busId @param {number} size  0..1 */
  setBusReverbRoomSize(busId, size) {}
  /** @param {number} busId @returns {number} */
  getBusReverbRoomSize(busId) {}
  /** @param {number} busId @param {number} damping  0..1 */
  setBusReverbDamping(busId, damping) {}
  /** @param {number} busId @returns {number} */
  getBusReverbDamping(busId) {}
  /** @param {number} busId @param {number} mix  0..1 */
  setBusReverbMix(busId, mix) {}
  /** @param {number} busId @returns {number} */
  getBusReverbMix(busId) {}

  /** @param {number} busId @param {boolean} enabled */
  setBusChorusEnabled(busId, enabled) {}
  /** @param {number} busId @returns {boolean} */
  getBusChorusEnabled(busId) {}
  /** @param {number} busId @param {number} hz  0.01..20 */
  setBusChorusRate(busId, hz) {}
  /** @param {number} busId @returns {number} */
  getBusChorusRate(busId) {}
  /** @param {number} busId @param {number} seconds  Modulation depth, 0.0001..0.05 */
  setBusChorusDepth(busId, seconds) {}
  /** @param {number} busId @returns {number} */
  getBusChorusDepth(busId) {}
  /** @param {number} busId @param {number} mix  0..1 */
  setBusChorusMix(busId, mix) {}
  /** @param {number} busId @returns {number} */
  getBusChorusMix(busId) {}
  /** @param {number} busId @param {number} feedback  0..0.95 */
  setBusChorusFeedback(busId, feedback) {}
  /** @param {number} busId @returns {number} */
  getBusChorusFeedback(busId) {}
  /** @param {number} busId @param {number} seconds  0.001..0.05 */
  setBusChorusBaseDelay(busId, seconds) {}
  /** @param {number} busId @returns {number} */
  getBusChorusBaseDelay(busId) {}

  /** @param {number} busId @param {boolean} enabled */
  setBusCompressorEnabled(busId, enabled) {}
  /** @param {number} busId @returns {boolean} */
  getBusCompressorEnabled(busId) {}
  /** LINEAR amplitude 0..1, not dB. @param {number} busId @param {number} threshold */
  setBusCompressorThreshold(busId, threshold) {}
  /** @param {number} busId @returns {number} */
  getBusCompressorThreshold(busId) {}
  /** @param {number} busId @param {number} ratio  1..20 */
  setBusCompressorRatio(busId, ratio) {}
  /** @param {number} busId @returns {number} */
  getBusCompressorRatio(busId) {}
  /** MILLISECONDS, 0.1..100. @param {number} busId @param {number} ms */
  setBusCompressorAttack(busId, ms) {}
  /** @param {number} busId @returns {number} */
  getBusCompressorAttack(busId) {}
  /** MILLISECONDS, 1..1000. @param {number} busId @param {number} ms */
  setBusCompressorRelease(busId, ms) {}
  /** @param {number} busId @returns {number} */
  getBusCompressorRelease(busId) {}
  /** Key the compressor from another bus's signal (-1, the default, = its own). @param {number} busId @param {number} sidechainBusId */
  setBusCompressorSidechain(busId, sidechainBusId) {}
  /** @param {number} busId @returns {number} */
  getBusCompressorSidechain(busId) {}

  /** 7-band graphic EQ at 60, 170, 350, 1k, 3.5k, 10k, 16k Hz. @param {number} busId @param {boolean} enabled */
  setBusEqEnabled(busId, enabled) {}
  /** @param {number} busId @returns {boolean} */
  getBusEqEnabled(busId) {}
  /** @param {number} busId @param {number} band  0..6 @param {number} dB  -12..12 */
  setBusEqBandGain(busId, band, dB) {}
  /** @param {number} busId @param {number} band @returns {number} */
  getBusEqBandGain(busId, band) {}
  /** @param {number} busId @param {number} dB  0..11 */
  setBusEqMasterGain(busId, dB) {}
  /** @param {number} busId @returns {number} */
  getBusEqMasterGain(busId) {}

  /** @param {number} busId @param {boolean} enabled */
  setBusDistortionEnabled(busId, enabled) {}
  /** @param {number} busId @returns {boolean} */
  getBusDistortionEnabled(busId) {}
  /** @param {number} busId @param {string} mode  'softclip' (default for unknown) | 'hardclip' | 'foldback' | 'bitcrush' */
  setBusDistortionMode(busId, mode) {}
  /** @param {number} busId @returns {string} */
  getBusDistortionMode(busId) {}
  /** @param {number} busId @param {number} drive  0.1..100 */
  setBusDistortionDrive(busId, drive) {}
  /** @param {number} busId @returns {number} */
  getBusDistortionDrive(busId) {}
  /** @param {number} busId @param {number} mix  0..1 */
  setBusDistortionMix(busId, mix) {}
  /** @param {number} busId @returns {number} */
  getBusDistortionMix(busId) {}
  /** @param {number} busId @param {number} gain  0..2 */
  setBusDistortionOutputGain(busId, gain) {}
  /** @param {number} busId @returns {number} */
  getBusDistortionOutputGain(busId) {}
  /** Bitcrush depth. @param {number} busId @param {number} bits  1..16 */
  setBusDistortionCrushBits(busId, bits) {}
  /** @param {number} busId @returns {number} */
  getBusDistortionCrushBits(busId) {}
  /** Bitcrush sample-rate fraction. @param {number} busId @param {number} rate  0.01..1 */
  setBusDistortionCrushRate(busId, rate) {}
  /** @param {number} busId @returns {number} */
  getBusDistortionCrushRate(busId) {}

  /**
   * Reorders a bus's effect chain. `order` lists 1..7 slot names in
   * processing order: 'filter', 'delay', 'compressor', 'chorus', 'reverb',
   * 'equalizer' (or 'eq'), 'distortion'. An unknown name keeps that
   * position's default slot (the default order is the list above). Empty or
   * longer lists are ignored.
   * @param {number} busId
   * @param {Array<string>} order
   */
  setBusEffectOrder(busId, order) {}

  /**
   * Runs mono samples through a copy of a bus's effect chain without
   * touching live audio.
   * @param {number} busId
   * @param {Float32Array|ArrayBuffer} samples  A Float32Array, or an
   *   ArrayBuffer read as float32. TypeError for another typed array or a
   *   detached one.
   * @returns {Float32Array|null} null for missing or empty input
   */
  processEffectsOffline(busId, samples) {}

  /** Compile the bus's effect chain to native code. @param {number} busId @param {boolean} enabled */
  setBusJitEnabled(busId, enabled) {}
  /** @param {number} busId @returns {boolean} */
  getBusJitEnabled(busId) {}
  /** Same as getBusJitEnabled. @param {number} busId @returns {boolean} */
  isBusJitEnabled(busId) {}
  /** Whether compiled code is running now. @param {number} busId @returns {boolean} */
  getBusJitActive(busId) {}
  /** Same as getBusJitActive. @param {number} busId @returns {boolean} */
  isBusJitActive(busId) {}

  // ── Master bus shortcuts ────────────────────────────────────────────────
  // Bus-0 versions of the bus effect setters, same units and ranges.

  /** @returns {number} master filter slot, or -1 (the slots BiquadFilterNodes use) */
  allocateFilterSlot() {}
  /** @param {number} slot */
  releaseFilterSlot(slot) {}
  /** @param {number} slot @param {boolean} enabled */
  setFilterEnabled(slot, enabled) {}
  /** @param {number} slot @param {string} type */
  setFilterType(slot, type) {}
  /** @param {number} slot @param {number} hz */
  setFilterFrequency(slot, hz) {}
  /** @param {number} slot @param {number} q */
  setFilterQ(slot, q) {}
  /** @param {number} slot @param {number} dB */
  setFilterGain(slot, dB) {}

  /** @param {boolean} enabled */
  setDelayEnabled(enabled) {}
  /** @param {number} seconds */
  setDelayTime(seconds) {}
  /** @param {number} feedback */
  setDelayFeedback(feedback) {}
  /** @param {number} mix */
  setDelayMix(mix) {}

  /** @param {boolean} enabled */
  setReverbEnabled(enabled) {}
  /** @param {number} size */
  setReverbRoomSize(size) {}
  /** @param {number} damping */
  setReverbDamping(damping) {}
  /** @param {number} mix */
  setReverbMix(mix) {}

  /** @param {boolean} enabled */
  setChorusEnabled(enabled) {}
  /** @param {number} hz */
  setChorusRate(hz) {}
  /** @param {number} seconds */
  setChorusDepth(seconds) {}
  /** @param {number} mix */
  setChorusMix(mix) {}
  /** @param {number} feedback */
  setChorusFeedback(feedback) {}
  /** @param {number} seconds */
  setChorusBaseDelay(seconds) {}

  /** @param {boolean} enabled */
  setCompressorEnabled(enabled) {}
  /** Linear 0..1, not dB. @param {number} threshold */
  setCompressorThreshold(threshold) {}
  /** @param {number} ratio */
  setCompressorRatio(ratio) {}
  /** Milliseconds. @param {number} ms */
  setCompressorAttack(ms) {}
  /** Milliseconds. @param {number} ms */
  setCompressorRelease(ms) {}

  /** The master output limiter (on by default). @param {boolean} enabled */
  setLimiterEnabled(enabled) {}
  /** @param {number} dB  Default -6. */
  setLimiterThreshold(dB) {}
  /** @param {number} ms  Default 50. */
  setLimiterRelease(ms) {}

  // ── Listener & head model ───────────────────────────────────────────────

  /** Same as listener.setPosition. @param {number} x @param {number} y @param {number} z */
  setListenerPosition(x, y, z) {}
  /** Same as listener.setOrientation (forward, then up). */
  setListenerOrientation(fx, fy, fz, ux, uy, uz) {}
  /** Same as listener.setVelocity. @param {number} x @param {number} y @param {number} z */
  setListenerVelocity(x, y, z) {}

  /** Head-shadow filtering of spatial sources (default on); distance and pan apply either way. @param {boolean} enabled */
  setHeadModelEnabled(enabled) {}
  /** Far-ear gain reduction at 90 degrees, default 0.85. @param {number} strength */
  setHeadModelIldStrength(strength) {}
  /** Gain reduction for a source directly behind, default 0.45. @param {number} attenuation */
  setHeadModelBehindAttenuation(attenuation) {}
  /** Near-ear cutoff for a source in front / behind, defaults 18000 / 2000 Hz. @param {number} frontHz @param {number} behindHz */
  setHeadModelNearCutoff(frontHz, behindHz) {}
  /** How far the far-ear cutoff drops at 90 degrees, default 0.95. @param {number} ratio */
  setHeadModelFarCutoffRatio(ratio) {}
  /** Cutoff shift per unit elevation, defaults 5000 / 2000 Hz. @param {number} nearHz @param {number} farHz */
  setHeadModelElevation(nearHz, farHz) {}
  /** Cutoff clamps, defaults 200 / 20000 Hz. @param {number} minHz @param {number} maxHz */
  setHeadModelCutoffRange(minHz, maxHz) {}

  // ── Offline rendering & analysis ────────────────────────────────────────

  /**
   * Renders `numFrames` through the whole pipeline without a device and
   * returns the latest mono mixdown: a new Float32Array of
   * min(numFrames, 16384) frames, or `out` filled in place up to its length.
   * `out` must be a Float32Array when given (null or undefined means a new
   * array); anything else throws TypeError before any frame is rendered.
   * Undefined for a missing or non-positive `numFrames`. Headless use only:
   * it races a live device.
   *
   * It renders in 128-frame quanta and evaluates AudioParam automation
   * before each one, so a ramp moves within the block. When the block is
   * done it runs the same tick as the host frame (automation, then
   * `onended` for buffer sources that ended in the block), so a headless
   * driver that only calls renderBlock still gets those events.
   * @param {number} numFrames
   * @param {Float32Array|null} [out]
   * @returns {Float32Array|undefined}
   */
  renderBlock(numFrames, out) {}

  /**
   * Linear magnitude spectrum (Hann window) of the latest output,
   * `numBins` values over 0..Nyquist.
   * @param {number} numBins  1..8192
   * @returns {Float32Array|undefined} undefined outside that range
   */
  getSpectrum(numBins) {}

  // ── Presets ─────────────────────────────────────────────────────────────
  // Presets are plain objects on the JS side and JSON strings on disk. The
  // *ToJson functions take an object and return its JSON string; the
  // *FromJson functions parse a JSON string into a full object (every field
  // present, defaults filled in). Both return null with no argument. The
  // apply* functions take objects; there is no getter that reads the live
  // engine state back into a preset.

  /** @param {VoicePreset} preset @returns {string|null} */
  voicePresetToJson(preset) {}
  /** @param {string} json @returns {VoicePreset|null} */
  voicePresetFromJson(json) {}
  /** @param {BusPreset} preset @returns {string|null} */
  busPresetToJson(preset) {}
  /** @param {string} json @returns {BusPreset|null} */
  busPresetFromJson(json) {}
  /** @param {ModPreset} preset @returns {string|null} */
  modPresetToJson(preset) {}
  /** @param {string} json @returns {ModPreset|null} */
  modPresetFromJson(json) {}
  /** @param {EnginePreset} preset @returns {string|null} */
  enginePresetToJson(preset) {}
  /** @param {string} json @returns {EnginePreset|null} */
  enginePresetFromJson(json) {}

  /** Needs both arguments. @param {number} voiceId @param {VoicePreset} preset */
  applyVoicePreset(voiceId, preset) {}
  /** Needs both arguments. @param {number} busId @param {BusPreset} preset */
  applyBusPreset(busId, preset) {}
  /** Applies to the engine-wide ModMatrix. @param {ModPreset} preset */
  applyModPreset(preset) {}
  /** @param {EnginePreset} preset */
  applyEnginePreset(preset) {}

  /**
   * Writes a preset's JSON string to `path`. Needs both arguments.
   * @param {string} json
   * @param {string} path
   * @returns {boolean}
   */
  savePreset(json, path) {}

  /**
   * @param {string} path
   * @returns {string|null} the JSON string; null when the file is missing or empty
   */
  loadPreset(path) {}

}
