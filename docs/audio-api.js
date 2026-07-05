// =============================================================================
// bro Audio API Reference
// =============================================================================
//
// The audio API is available to all bro apps via the global `AudioContext`
// constructor. It wraps the broaudio C++ engine through QuickJS bindings and
// provides a Web Audio API-inspired interface plus synth, sequencing, spatial
// audio, and mix-bus features.
//
// Obtain the context:
//   const ctx = new AudioContext();
//
// Microphone access (returns a Promise<MediaStream>):
//   const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
//
// =============================================================================


// -----------------------------------------------------------------------------
// AudioContext
// -----------------------------------------------------------------------------

class AudioContext {

  // --- Properties -----------------------------------------------------------

  /** Current audio engine time in seconds (read-only). */
  get currentTime() {}

  /** Sample rate of the audio engine, e.g. 44100 or 48000 (read-only). */
  get sampleRate() {}

  /** Master output gain (0.0 - 1.0+). */
  get masterGain() {}
  set masterGain(value) {}

  /** Whether the microphone input is muted. */
  get micMuted() {}
  set micMuted(value) {}

  /** Mic monitoring gain (0.0 - 1.0). Controls how loud the mic is in the output. */
  get micMonitorGain() {}
  set micMonitorGain(value) {}

  /** Bus ID the microphone is routed to. */
  get micBus() {}
  set micBus(busId) {}

  /** Whether the engine is currently recording (read-only). */
  get recording() {}

  /** The AudioDestinationNode representing the final output (read-only). */
  get destination() {}


  // --- Voice Lifecycle (raw ID) ---------------------------------------------

  /**
   * Create a voice and return its integer ID.
   * Unlike createOscillator() which returns an OscillatorNode wrapper,
   * this returns a raw ID for use with the direct voice parameter APIs.
   * @returns {number} voiceId
   */
  createVoice() {}

  /** @param {number} voiceId */ removeVoice(voiceId) {}

  /**
   * Start a voice's envelope.
   * @param {number} voiceId
   * @param {number} when - engine time in seconds
   */
  startVoice(voiceId, when) {}

  /**
   * Stop a voice (trigger release).
   * @param {number} voiceId
   * @param {number} when - engine time in seconds
   */
  stopVoice(voiceId, when) {}

  /**
   * Mark a voice as persistent (not auto-purged when envelope finishes).
   * @param {number} voiceId
   * @param {boolean} persistent
   */
  setVoicePersistent(voiceId, persistent) {}


  // --- Node Creation --------------------------------------------------------

  /**
   * Create an oscillator voice wrapped in an OscillatorNode.
   * @returns {OscillatorNode}
   */
  createOscillator() {}

  /**
   * Create a gain node.
   * @returns {GainNode}
   */
  createGain() {}

  /**
   * Create an analyser for FFT / waveform visualization.
   * @returns {AnalyserNode}
   */
  createAnalyser() {}

  /**
   * Create a biquad filter node (lowpass, highpass, bandpass, etc.).
   * Limited by available filter slots in the engine.
   * @returns {BiquadFilterNode}
   * @throws if no filter slots are available
   */
  createBiquadFilter() {}

  /**
   * Create a source node from a MediaStream (microphone).
   * @param {MediaStream} stream - from navigator.mediaDevices.getUserMedia()
   * @returns {MediaStreamAudioSourceNode}
   */
  createMediaStreamSource(stream) {}


  // --- Factory Methods ------------------------------------------------------

  /**
   * Create a polyphonic voice allocator.
   * @param {number} [maxVoices=16] - max simultaneous voices
   * @returns {VoiceAllocator}
   */
  createVoiceAllocator(maxVoices) {}

  /**
   * Get the engine's global modulation matrix.
   * @returns {ModMatrix}
   */
  getModMatrix() {}

  /**
   * Create a MIDI input handler.
   * @returns {MidiInput}
   */
  createMidiInput() {}

  /**
   * Create a note sequence tied to a VoiceAllocator.
   * @param {VoiceAllocator} allocator
   * @returns {Sequence}
   */
  createSequence(allocator) {}


  // --- Master Effects (shorthand for bus 0) ---------------------------------

  // Delay
  /** @param {boolean} enabled */ setDelayEnabled(enabled) {}
  /** @param {number} seconds */ setDelayTime(seconds) {}
  /** @param {number} amount - 0.0 to 1.0 */ setDelayFeedback(amount) {}
  /** @param {number} mix - 0.0 (dry) to 1.0 (wet) */ setDelayMix(mix) {}

  // Reverb
  /** @param {boolean} enabled */ setReverbEnabled(enabled) {}
  /** @param {number} size - 0.0 to 1.0 */ setReverbRoomSize(size) {}
  /** @param {number} damping - 0.0 to 1.0 */ setReverbDamping(damping) {}
  /** @param {number} mix */ setReverbMix(mix) {}

  // Chorus
  /** @param {boolean} enabled */ setChorusEnabled(enabled) {}
  /** @param {number} hz */ setChorusRate(hz) {}
  /** @param {number} depth */ setChorusDepth(depth) {}
  /** @param {number} mix */ setChorusMix(mix) {}
  /** @param {number} feedback */ setChorusFeedback(feedback) {}
  /** @param {number} seconds */ setChorusBaseDelay(seconds) {}

  // Compressor
  /** @param {boolean} enabled */ setCompressorEnabled(enabled) {}
  /** @param {number} dB */ setCompressorThreshold(dB) {}
  /** @param {number} ratio */ setCompressorRatio(ratio) {}
  /** @param {number} ms - attack time in milliseconds, clamped to [0.1, 100] */ setCompressorAttack(ms) {}
  /** @param {number} ms - release time in milliseconds, clamped to [1, 1000] */ setCompressorRelease(ms) {}

  // Master limiter — a lookahead peak limiter on the master bus, applied after
  // master gain. Catches inter-sample peaks the compressor misses; keep it on
  // to prevent clipping when many voices stack up.
  //   ctx.setLimiterEnabled(true);
  //   ctx.setLimiterThreshold(-1.0);   // ceiling in dBFS
  //   ctx.setLimiterRelease(80);       // ms
  /** @param {boolean} enabled */ setLimiterEnabled(enabled) {}
  /** @param {number} dB - ceiling in dBFS (e.g. -1.0); peaks above are limited */ setLimiterThreshold(dB) {}
  /** @param {number} ms - gain-recovery release time in milliseconds */ setLimiterRelease(ms) {}


  // --- Mix Bus API ----------------------------------------------------------

  /**
   * Create a new mix bus.
   * @returns {number} busId
   */
  createBus() {}

  /** @param {number} busId */ deleteBus(busId) {}
  /** @param {number} busId @param {number} gain */ setBusGain(busId, gain) {}
  /** @param {number} busId @param {number} pan - -1.0 (left) to 1.0 (right) */ setBusPan(busId, pan) {}
  /** @param {number} busId @param {boolean} muted */ setBusMuted(busId, muted) {}

  // Per-bus filters
  /** @param {number} busId @returns {number} slot */ allocateBusFilterSlot(busId) {}
  /** @param {number} busId @param {number} slot */ releaseBusFilterSlot(busId, slot) {}
  /** @param {number} busId @param {number} slot @param {boolean} enabled */ setBusFilterEnabled(busId, slot, enabled) {}
  /**
   * @param {number} busId
   * @param {number} slot
   * @param {string} type - "lowpass"|"highpass"|"bandpass"|"notch"|"allpass"|"peaking"|"lowshelf"|"highshelf"
   */
  setBusFilterType(busId, slot, type) {}
  /** @param {number} busId @param {number} slot @param {number} hz */ setBusFilterFrequency(busId, slot, hz) {}
  /** @param {number} busId @param {number} slot @param {number} q */ setBusFilterQ(busId, slot, q) {}
  /** @param {number} busId @param {number} slot @param {number} dB */ setBusFilterGain(busId, slot, dB) {}

  // Per-bus effects
  /** @param {number} busId @param {boolean} enabled */ setBusDelayEnabled(busId, enabled) {}
  /** @param {number} busId @param {number} seconds */ setBusDelayTime(busId, seconds) {}
  /** @param {number} busId @param {number} amount */ setBusDelayFeedback(busId, amount) {}
  /** @param {number} busId @param {number} mix */ setBusDelayMix(busId, mix) {}

  /** @param {number} busId @param {boolean} enabled */ setBusCompressorEnabled(busId, enabled) {}
  /** @param {number} busId @param {number} dB */ setBusCompressorThreshold(busId, dB) {}
  /** @param {number} busId @param {number} ratio */ setBusCompressorRatio(busId, ratio) {}
  /** @param {number} busId @param {number} ms - attack time in milliseconds, clamped to [0.1, 100] */ setBusCompressorAttack(busId, ms) {}
  /** @param {number} busId @param {number} ms - release time in milliseconds, clamped to [1, 1000] */ setBusCompressorRelease(busId, ms) {}
  /** @param {number} busId @param {number} sidechainBusId */ setBusCompressorSidechain(busId, sidechainBusId) {}

  /** @param {number} busId @param {boolean} enabled */ setBusReverbEnabled(busId, enabled) {}
  /** @param {number} busId @param {number} size */ setBusReverbRoomSize(busId, size) {}
  /** @param {number} busId @param {number} damping */ setBusReverbDamping(busId, damping) {}
  /** @param {number} busId @param {number} mix */ setBusReverbMix(busId, mix) {}

  /** @param {number} busId @param {boolean} enabled */ setBusChorusEnabled(busId, enabled) {}
  /** @param {number} busId @param {number} hz */ setBusChorusRate(busId, hz) {}
  /** @param {number} busId @param {number} depth */ setBusChorusDepth(busId, depth) {}
  /** @param {number} busId @param {number} mix */ setBusChorusMix(busId, mix) {}
  /** @param {number} busId @param {number} feedback */ setBusChorusFeedback(busId, feedback) {}
  /** @param {number} busId @param {number} seconds */ setBusChorusBaseDelay(busId, seconds) {}

  // Per-bus 7-band parametric EQ
  // Bands are fixed peaking filters at: 60, 170, 350, 1000, 3500, 10000, 16000 Hz.
  // Band index is 0-6; gain is in dB (typical range -24..+24).
  /** @param {number} busId @param {boolean} enabled */ setBusEqEnabled(busId, enabled) {}
  /** @param {number} busId @param {number} dB - master output trim in dB */ setBusEqMasterGain(busId, dB) {}
  /** @param {number} busId @param {number} band - 0..6 @param {number} dB */ setBusEqBandGain(busId, band, dB) {}

  // Per-bus Distortion (waveshaper / bitcrusher)
  /** @param {number} busId @param {boolean} enabled */ setBusDistortionEnabled(busId, enabled) {}
  /**
   * @param {number} busId
   * @param {string} mode - "softclip" (tanh) | "hardclip" | "foldback" | "bitcrush"
   */
  setBusDistortionMode(busId, mode) {}
  /** @param {number} busId @param {number} drive - 1.0 = unity, higher = more saturation */ setBusDistortionDrive(busId, drive) {}
  /** @param {number} busId @param {number} mix - 0=dry, 1=full wet */ setBusDistortionMix(busId, mix) {}
  /** @param {number} busId @param {number} gain - post-distortion output gain compensation */ setBusDistortionOutputGain(busId, gain) {}
  /** @param {number} busId @param {number} bits - bit depth for "bitcrush" mode (1-16) */ setBusDistortionCrushBits(busId, bits) {}
  /** @param {number} busId @param {number} rate - sample-rate factor for "bitcrush" (0.01-1.0) */ setBusDistortionCrushRate(busId, rate) {}

  /**
   * Set the effect processing order for a bus.
   * @param {number} busId
   * @param {string[]} order - e.g. ["filter", "delay", "compressor", "chorus", "reverb", "equalizer", "distortion"]
   */
  setBusEffectOrder(busId, order) {}

  // Bus metering (read current levels)
  /** @param {number} busId @returns {number} peak level, left channel */ getBusPeakL(busId) {}
  /** @param {number} busId @returns {number} peak level, right channel */ getBusPeakR(busId) {}
  /** @param {number} busId @returns {number} RMS level, left channel */ getBusRmsL(busId) {}
  /** @param {number} busId @returns {number} RMS level, right channel */ getBusRmsR(busId) {}


  // --- Aux Sends ------------------------------------------------------------

  /**
   * Route a voice's output to an aux bus.
   * @param {number} voiceId
   * @param {number} sendBusId - destination bus
   * @param {number} amount - send level (0.0 - 1.0)
   */
  setVoiceSend(voiceId, sendBusId, amount) {}

  /** @param {number} playbackId @param {number} sendBusId @param {number} amount */
  setPlaybackSend(playbackId, sendBusId, amount) {}

  /** @param {number} busId @param {number} sendBusId @param {number} amount */
  setBusSend(busId, sendBusId, amount) {}


  // --- Sample-Accurate Scheduling -------------------------------------------

  /** @param {number} voiceId @param {number} when - time in seconds */ scheduleNoteOn(voiceId, when) {}
  /** @param {number} voiceId @param {number} when - time in seconds */ scheduleNoteOff(voiceId, when) {}


  // --- Voice / Clip Bus Routing ---------------------------------------------

  /** @param {number} voiceId @param {number} busId */ setVoiceBus(voiceId, busId) {}
  /** @param {number} playbackId @param {number} busId */ setPlaybackBus(playbackId, busId) {}


  // --- Direct Voice Parameter Control ---------------------------------------
  // These are useful inside VoiceAllocator.setVoiceSetup() callbacks.

  /**
   * Set MIDI note context for a voice (used by modulation key-tracking).
   * @param {number} voiceId
   * @param {number} note - MIDI note number (0-127)
   * @param {number} velocity - 0.0 to 1.0
   */
  setVoiceNote(voiceId, note, velocity) {}

  /**
   * @param {number} voiceId
   * @param {string} waveform - "sine"|"square"|"sawtooth"|"triangle"|"wavetable"|"whitenoise"|"pinknoise"|"brownnoise"
   */
  setVoiceWaveform(voiceId, waveform) {}

  /** @param {number} voiceId @param {number} hz */ setVoiceFrequency(voiceId, hz) {}
  /** @param {number} voiceId @param {number} gain */ setVoiceGain(voiceId, gain) {}
  /** @param {number} voiceId @param {number} pan - -1.0 to 1.0 */ setVoicePan(voiceId, pan) {}
  /** @param {number} voiceId @param {number} seconds */ setVoiceAttack(voiceId, seconds) {}
  /** @param {number} voiceId @param {number} seconds */ setVoiceDecay(voiceId, seconds) {}
  /** @param {number} voiceId @param {number} level - 0.0 to 1.0 */ setVoiceSustain(voiceId, level) {}
  /** @param {number} voiceId @param {number} seconds */ setVoiceRelease(voiceId, seconds) {}

  /**
   * Pitch-bend a voice by a semitone offset (applied on top of its frequency).
   * Positive bends up, negative down; 0 is no bend. OscillatorNode also exposes
   * this as a `pitchBend` AudioParam (osc.pitchBend.value = 2).
   * @param {number} voiceId
   * @param {number} semitones - e.g. 2 = up a whole tone, -12 = down an octave
   */
  setVoicePitchBend(voiceId, semitones) {}

  // Unison
  /** @param {number} voiceId @param {number} count */ setVoiceUnisonCount(voiceId, count) {}
  /** @param {number} voiceId @param {number} cents */ setVoiceUnisonDetune(voiceId, cents) {}
  /** @param {number} voiceId @param {number} width - 0.0 to 1.0 */ setVoiceUnisonStereoWidth(voiceId, width) {}

  // Per-voice filter
  /** @param {number} voiceId @param {boolean} enabled */ setVoiceFilterEnabled(voiceId, enabled) {}
  /**
   * @param {number} voiceId
   * @param {string} type - "lowpass"|"highpass"|"bandpass"|"notch"|"allpass"|"peaking"|"lowshelf"|"highshelf"
   */
  setVoiceFilterType(voiceId, type) {}
  /** @param {number} voiceId @param {number} hz */ setVoiceFilterFrequency(voiceId, hz) {}
  /** @param {number} voiceId @param {number} q */ setVoiceFilterQ(voiceId, q) {}


  // --- Wavetable Synthesis --------------------------------------------------

  /**
   * Create a built-in wavetable bank.
   * @param {string} type - "saw"|"square"|"triangle"
   * @returns {number} wavetableId
   */
  createWavetable(type) {}

  /**
   * Create a wavetable from a single-cycle waveform.
   * @param {Float32Array} waveform - one cycle of audio data
   * @returns {number} wavetableId
   */
  createWavetableFromWaveform(waveform) {}

  /** @param {number} wavetableId */ deleteWavetable(wavetableId) {}

  /**
   * Assign a wavetable to a voice and switch it to wavetable mode.
   * @param {number} voiceId
   * @param {number} wavetableId
   */
  setVoiceWavetable(voiceId, wavetableId) {}


  // --- Spectrum API ---------------------------------------------------------

  /**
   * Get the current output spectrum as a Float32Array.
   * @param {number} numBins - number of frequency bins (max 8192)
   * @returns {Float32Array} magnitude data
   */
  getSpectrum(numBins) {}


  // --- Spatial Audio --------------------------------------------------------

  // Listener
  /** @param {number} x @param {number} y @param {number} z */
  setListenerPosition(x, y, z) {}

  /**
   * @param {number} forwardX @param {number} forwardY @param {number} forwardZ
   * @param {number} upX @param {number} upY @param {number} upZ
   */
  setListenerOrientation(forwardX, forwardY, forwardZ, upX, upY, upZ) {}

  // Voice spatial sources
  /** @param {number} voiceId @param {boolean} enabled */ setVoiceSpatialEnabled(voiceId, enabled) {}
  /** @param {number} voiceId @param {number} x @param {number} y @param {number} z */ setVoiceSpatialPosition(voiceId, x, y, z) {}
  /** @param {number} voiceId @param {number} distance */ setVoiceSpatialRefDistance(voiceId, distance) {}
  /** @param {number} voiceId @param {number} distance */ setVoiceSpatialMaxDistance(voiceId, distance) {}
  /** @param {number} voiceId @param {number} factor */ setVoiceSpatialRolloff(voiceId, factor) {}
  /** @param {number} voiceId @param {string} model - "inverse"|"linear"|"exponential" */ setVoiceSpatialDistanceModel(voiceId, model) {}

  // Playback spatial sources
  /** @param {number} playbackId @param {boolean} enabled */ setPlaybackSpatialEnabled(playbackId, enabled) {}
  /** @param {number} playbackId @param {number} x @param {number} y @param {number} z */ setPlaybackSpatialPosition(playbackId, x, y, z) {}
  /** @param {number} playbackId @param {number} distance */ setPlaybackSpatialRefDistance(playbackId, distance) {}
  /** @param {number} playbackId @param {number} distance */ setPlaybackSpatialMaxDistance(playbackId, distance) {}
  /** @param {number} playbackId @param {number} factor */ setPlaybackSpatialRolloff(playbackId, factor) {}
  /** @param {number} playbackId @param {string} model - "inverse"|"linear"|"exponential" */ setPlaybackSpatialDistanceModel(playbackId, model) {}


  // --- Head Model (spatial filtering) --------------------------------------
  // Configures the per-ear head-shadow model applied to all spatial sources.
  // When enabled, the head model replaces simple stereo panning with
  // physically-motivated ILD (level difference) and ITF (frequency filtering)
  // per ear, producing front/back, left/right, and elevation cues.

  /** Enable or disable the head shadow model. Default: true. */
  /** @param {boolean} enabled */ setHeadModelEnabled(enabled) {}

  /**
   * Far-ear gain reduction at 90 degrees off-axis.
   * 0.0 = no reduction (both ears equal), 1.0 = far ear silent.
   * @param {number} strength - default 0.85
   */
  setHeadModelIldStrength(strength) {}

  /**
   * Both-ear gain reduction when source is directly behind the listener.
   * 0.0 = no reduction, 1.0 = silent.
   * @param {number} attenuation - default 0.45
   */
  setHeadModelBehindAttenuation(attenuation) {}

  /**
   * Near-ear lowpass cutoff range in Hz, modulated by front/back angle.
   * Source directly in front uses frontHz, directly behind uses behindHz.
   * @param {number} frontHz - default 18000
   * @param {number} behindHz - default 2000
   */
  setHeadModelNearCutoff(frontHz, behindHz) {}

  /**
   * How aggressively the far ear's cutoff drops relative to the near ear.
   * At 90 degrees off-axis: farCutoff = nearCutoff * (1 - ratio).
   * 0.0 = far ear same as near, 1.0 = far ear cutoff drops to zero.
   * @param {number} ratio - default 0.95
   */
  setHeadModelFarCutoffRatio(ratio) {}

  /**
   * Elevation influence on filter cutoff. Sources above shift cutoff up
   * (brighter), below shift down (darker). Values are Hz shift per unit elevation.
   * @param {number} nearHz - near ear shift, default 5000
   * @param {number} farHz - far ear shift, default 2000
   */
  setHeadModelElevation(nearHz, farHz) {}

  /**
   * Hard clamps on computed cutoff frequencies.
   * @param {number} minHz - default 200
   * @param {number} maxHz - default 20000
   */
  setHeadModelCutoffRange(minHz, maxHz) {}


  // --- Recording ------------------------------------------------------------

  /** Start capturing the audio output. */
  startRecording() {}

  /**
   * Stop recording and return captured audio.
   * @returns {Float32Array|null} recorded samples, or null if nothing captured
   */
  stopRecording() {}

  /**
   * Export the current recording buffer directly to a WAV file (32-bit float).
   * Call after stopRecording(). More efficient than stopRecording() + saveWav()
   * since it avoids copying samples through JS.
   * @param {string} path - output file path
   * @returns {boolean} true on success
   */
  exportRecordingToWav(path) {}


  // --- Audio File I/O -------------------------------------------------------

  /**
   * Create a clip by loading an audio file. Supports WAV, FLAC, MP3, and
   * OGG/Opus. Automatically resamples to the engine sample rate using a
   * high-quality polyphase sinc resampler.
   * @param {string} path - file path
   * @returns {number} clipId, or -1 on failure
   */
  createClipFromFile(path) {}

  /**
   * Decode an audio file from a memory buffer. Supports WAV, FLAC, MP3, and
   * OGG/Opus (detected by header magic bytes). Automatically resamples to the
   * engine sample rate.
   * @param {ArrayBuffer|Uint8Array} data - raw file bytes
   * @returns {?{samples: Float32Array, channels: number, sampleRate: number, numFrames: number}}
   *          decoded audio data, or null on failure
   */
  decodeAudioData(data) {}

  /**
   * Decode an audio file from disk. Same format support and resampling as
   * decodeAudioData, but reads from a file path. Returns the decoded samples
   * for use in the clip editor or other processing.
   * @param {string} path - file path
   * @returns {?{samples: Float32Array, channels: number, sampleRate: number, numFrames: number}}
   *          decoded audio data, or null on failure
   */
  decodeAudioFile(path) {}

  /**
   * Save PCM audio data to a WAV file (32-bit float format).
   * @param {string} path - output file path
   * @param {Float32Array} samples - interleaved sample data
   * @param {number} channels - 1 for mono, 2 for stereo
   * @param {number} sampleRate - e.g. 44100
   * @returns {boolean} true on success
   */
  saveWav(path, samples, channels, sampleRate) {}


  // --- Audio Clips ----------------------------------------------------------

  /**
   * Create an audio clip from sample data.
   * @param {Float32Array} samples
   * @param {number} [channels=1] - 1 for mono, 2 for stereo (interleaved)
   * @param {number} [sampleRate] - source rate of `samples`; when it differs
   *   from the engine rate the clip is resampled so it plays at the right
   *   pitch/speed. Omit when `samples` are already at the engine rate.
   * @returns {number} clipId
   */
  createClip(samples, channels, sampleRate) {}

  /** @param {number} clipId */ deleteClip(clipId) {}
  /** @param {number} clipId @returns {number} */ getClipSampleCount(clipId) {}
  /** @param {number} clipId @returns {number} */ getClipChannels(clipId) {}

  /**
   * Get a downsampled waveform for display.
   * @param {number} clipId
   * @param {number} numBins - number of min/max pairs (max 1024)
   * @returns {Float32Array} interleaved [min0, max0, min1, max1, ...] with length numBins*2
   */
  getClipWaveform(clipId, numBins) {}


  // --- Clip Playback --------------------------------------------------------

  /**
   * Play an audio clip.
   * @param {number} clipId
   * @param {number} [gain=1.0]
   * @param {boolean} [loop=false]
   * @param {number} [when] - sample-accurate start time in engine seconds
   *   (from ctx.currentTime). When given, the clip is scheduled on the audio
   *   clock instead of starting immediately, so back-to-back calls (e.g.
   *   streaming fixed-size chunks) join gaplessly with no main-thread
   *   setTimeout jitter or clock drift. A `when` at/before now plays
   *   immediately, same as the 3-arg form. This is playClipAt() — same
   *   underlying method, just called with a 4th argument.
   * @returns {number} playbackId - handle for controlling this playback instance
   *
   * @example
   *   const t0 = ctx.currentTime;
   *   ctx.playClip(clipA, 1.0, false, t0);
   *   ctx.playClip(clipB, 1.0, false, t0 + clipADuration); // gapless join
   */
  playClip(clipId, gain, loop, when) {}

  /** @param {number} playbackId */ stopPlayback(playbackId) {}
  /** @param {number} playbackId @param {number} gain */ setPlaybackGain(playbackId, gain) {}
  /** @param {number} playbackId @param {boolean} loop */ setPlaybackLoop(playbackId, loop) {}
  /** @param {number} playbackId @param {boolean} playing */ setPlaybackPlaying(playbackId, playing) {}
  /**
   * Set the playback region (sub-range of the clip).
   * @param {number} playbackId
   * @param {number} startSample
   * @param {number} endSample
   */
  setPlaybackRegion(playbackId, startSample, endSample) {}
  /** @param {number} playbackId @param {number} rate - 1.0 = normal speed */ setPlaybackRate(playbackId, rate) {}
  /** @param {number} playbackId @param {number} pan - -1.0 to 1.0 */ setPlaybackPan(playbackId, pan) {}
  /** @param {number} playbackId @returns {number} normalized position in the clip/region, [0,1) — multiply by the clip duration for seconds */ getPlaybackPosition(playbackId) {}


  // --- Streaming PCM Source (live voice / network audio) --------------------

  /**
   * Create a streaming PCM source — a persistent, spatializable playback fed
   * sample data frame-by-frame instead of from a fixed clip. The returned id is
   * an ordinary playbackId: drive it with every setPlayback* / setPlaybackSpatial*
   * method (gain, pan, bus, sends, 3D position). The audio thread reads a ring
   * the producer appends to, so a continuous stream plays click-free without the
   * per-frame clip churn of createClip+playClip. This is the sink for Steam Voice
   * / network voice chat: one stream per remote speaker, positioned in space.
   *
   *   const s = ctx.createStream(1);                 // mono voice source
   *   ctx.setPlaybackSpatialEnabled(s, true);
   *   ctx.setPlaybackSpatialPosition(s, x, y, z);    // where the speaker is
   *   bro.steam.onvoicecaptured = bytes => peer.send(bytes);
   *   // on the receiving side, per packet:
   *   const { pcm } = await bro.steam.decodeVoice(bytes, ctx.sampleRate);
   *   ctx.pushStreamSamples(s, pcm);
   *
   * @param {number} [channels=1] - 1 mono, 2 stereo (interleaved)
   * @param {number} [ringFrames=0] - ring capacity in frames; 0 = ~2s at engine rate
   * @returns {number} streamId (a playbackId), or -1 on bad args
   */
  createStream(channels, ringFrames) {}

  /**
   * Append interleaved PCM to a streaming source. Samples MUST be at the engine
   * sample rate (resample on the producer side — e.g. decode voice directly at
   * ctx.sampleRate). Single-producer: push from one thread. On underrun the
   * source plays silence; on overrun (producer far ahead of playback) the oldest
   * audio is dropped to bound latency.
   * @param {number} streamId
   * @param {Float32Array} samples - interleaved, engine-rate
   * @returns {number} frames written
   */
  pushStreamSamples(streamId, samples) {}

  /** Stop and remove a streaming source. @param {number} streamId */
  closeStream(streamId) {}


  // --- Offline Processing ---------------------------------------------------

  /**
   * Process audio data through a bus's effect chain (non-realtime).
   * @param {number} busId
   * @param {Float32Array} input
   * @returns {Float32Array} processed output
   */
  processEffectsOffline(busId, input) {}

  /**
   * Render `numFrames` through the full pipeline (voices, clips, bus effects,
   * mix, limiter) WITHOUT a playback device, then return the latest mono
   * mixdown as a Float32Array. This is the offline/headless pump — in
   * bro-headless there is no audio device thread, so call this (or advanceTime,
   * which renders for you) to advance audio. Do NOT call it in a windowed app
   * with a live device: it would race the audio callback.
   *
   * @param {number} numFrames - frames to render (capped to the analysis ring)
   * @param {Float32Array} [out] - optional buffer to fill in place (up to its
   *        length); when omitted a fresh Float32Array(numFrames) is returned
   * @returns {Float32Array} the rendered mono samples
   *
   * @example
   *   const v = ctx.createVoice();
   *   ctx.setVoiceFrequency(v, 440);
   *   ctx.startVoice(v, ctx.currentTime);
   *   const pcm = ctx.renderBlock(4096);   // non-zero samples once the voice rings
   */
  renderBlock(numFrames, out) {}


  // --- Presets (serialization) ----------------------------------------------
  //
  // Presets are plain JS objects that mirror broaudio's preset structs. Build
  // one in JS (or load JSON), serialize/save it, and later load/deserialize and
  // apply it to the live engine. Enum fields use the same lowercase strings as
  // the rest of this API (waveform, filter type, distortion mode, LFO shape,
  // mod source/dest, effect-order slot names).
  //
  //   VoicePreset { waveform, frequency, gain, pan, pitchBend, attackTime,
  //                 decayTime, sustainLevel, releaseTime, filterEnabled,
  //                 filterType, filterFreq, filterQ, unisonCount, unisonDetune,
  //                 unisonStereoWidth }
  //   BusPreset   { gain, pan, effectOrder:[slot...], filters:[FilterPreset x4],
  //                 delay, compressor, reverb, chorus, distortion, eq }
  //   ModPreset   { lfos:[LfoPreset x4], routes:[RoutePreset...] }
  //   EnginePreset{ masterGain, limiter:{enabled,thresholdDb,releaseMs},
  //                 masterBus:BusPreset, buses:[BusPreset...], modulation:ModPreset }
  //
  //   const patch = { waveform: "sawtooth", filterEnabled: true, filterFreq: 800 };
  //   const json  = ctx.voicePresetToJson(patch);
  //   ctx.savePreset(json, "lead.json");
  //   const back  = ctx.voicePresetFromJson(ctx.loadPreset("lead.json"));
  //   ctx.applyVoicePreset(myVoiceId, back);

  /** @param {object} voicePreset @returns {string} JSON */ voicePresetToJson(voicePreset) {}
  /** @param {object} busPreset @returns {string} JSON */ busPresetToJson(busPreset) {}
  /** @param {object} modPreset @returns {string} JSON */ modPresetToJson(modPreset) {}
  /** @param {object} enginePreset @returns {string} JSON */ enginePresetToJson(enginePreset) {}

  /** @param {string} json @returns {object} voice preset */ voicePresetFromJson(json) {}
  /** @param {string} json @returns {object} bus preset */ busPresetFromJson(json) {}
  /** @param {string} json @returns {object} mod preset */ modPresetFromJson(json) {}
  /** @param {string} json @returns {object} engine preset */ enginePresetFromJson(json) {}

  /** @param {number} voiceId @param {object} voicePreset */ applyVoicePreset(voiceId, voicePreset) {}
  /** @param {number} busId @param {object} busPreset */ applyBusPreset(busId, busPreset) {}
  /** @param {object} modPreset - applied to the shared ModMatrix */ applyModPreset(modPreset) {}
  /** @param {object} enginePreset - master gain, limiter, buses, modulation */ applyEnginePreset(enginePreset) {}

  /**
   * Write a preset JSON string to a file.
   * @param {string} json @param {string} path @returns {boolean} success
   */
  savePreset(json, path) {}

  /**
   * Read a preset JSON string from a file.
   * @param {string} path @returns {string|null} the JSON, or null if not readable
   */
  loadPreset(path) {}
}


// -----------------------------------------------------------------------------
// OscillatorNode
// -----------------------------------------------------------------------------
// Created via `ctx.createOscillator()`. Wraps one engine voice.

class OscillatorNode {

  /**
   * Waveform type.
   * @type {string} "sine"|"square"|"sawtooth"|"triangle"|"wavetable"|"whitenoise"|"pinknoise"|"brownnoise"
   */
  get type() {}
  set type(value) {}

  /** @type {AudioParam} Frequency in Hz (default 440). */
  frequency;

  /** @type {AudioParam} Stereo pan, -1.0 (left) to 1.0 (right). */
  pan;

  /** @type {AudioParam} ADSR attack time in seconds (default 0.01). */
  attack;

  /** @type {AudioParam} ADSR decay time in seconds (default 0.1). */
  decay;

  /** @type {AudioParam} ADSR sustain level, 0.0-1.0 (default 1.0). */
  sustain;

  /** @type {AudioParam} ADSR release time in seconds (default 0.04). */
  release;

  /** @type {AudioParam} Pitch bend in semitones, applied on top of frequency (default 0). */
  pitchBend;

  /**
   * Connect this oscillator to another node (typically a GainNode or the destination).
   * @param {AudioNode} destination
   * @returns {AudioNode} the destination (for chaining)
   */
  connect(destination) {}

  disconnect() {}

  /**
   * Start the oscillator.
   * @param {number} [when=currentTime] - start time in seconds
   */
  start(when) {}

  /**
   * Stop the oscillator.
   * @param {number} [when=currentTime] - stop time in seconds
   */
  stop(when) {}
}


// -----------------------------------------------------------------------------
// GainNode
// -----------------------------------------------------------------------------
// Created via `ctx.createGain()`.

class GainNode {

  /** @type {{ value: number }} Gain amount (default 1.0). */
  gain;

  /** @param {AudioNode} destination @returns {AudioNode} */ connect(destination) {}
  disconnect() {}
}


// -----------------------------------------------------------------------------
// BiquadFilterNode
// -----------------------------------------------------------------------------
// Created via `ctx.createBiquadFilter()`. Uses an engine filter slot.

class BiquadFilterNode {

  /**
   * Filter type.
   * @type {string} "lowpass"|"highpass"|"bandpass"|"notch"|"allpass"|"peaking"|"lowshelf"|"highshelf"
   */
  get type() {}
  set type(value) {}

  /** @type {AudioParam} Cutoff/center frequency in Hz (default 1000). */
  frequency;

  /** @type {AudioParam} Quality factor (default 1.0). */
  Q;

  /** @type {AudioParam} Gain in dB for peaking/shelf filters (default 0). */
  gain;

  /**
   * Connect (enables the filter in the engine).
   * @param {AudioNode} destination
   * @returns {AudioNode}
   */
  connect(destination) {}

  /** Disconnect (disables the filter in the engine). */
  disconnect() {}
}


// -----------------------------------------------------------------------------
// AnalyserNode
// -----------------------------------------------------------------------------
// Created via `ctx.createAnalyser()`. Performs FFT analysis on audio buffers.

class AnalyserNode {

  /** FFT size (must be power of 2, 32-32768). Default 2048. */
  get fftSize() {}
  set fftSize(value) {}

  /** Number of frequency bins = fftSize / 2 (read-only). */
  get frequencyBinCount() {}

  /** Minimum dB value for byte scaling (default -100). */
  get minDecibels() {}
  set minDecibels(value) {}

  /** Maximum dB value for byte scaling (default -30). */
  get maxDecibels() {}
  set maxDecibels(value) {}

  /** Smoothing between FFT frames, 0.0-1.0 (default 0.8). */
  get smoothingTimeConstant() {}
  set smoothingTimeConstant(value) {}

  /**
   * Audio source selection.
   *   0 = engine output (default)
   *   1 = microphone
   *   2 = both combined
   */
  get source() {}
  set source(value) {}

  /**
   * Fill a Float32Array with frequency-domain data in dB.
   * @param {Float32Array} array - length should be >= frequencyBinCount
   */
  getFloatFrequencyData(array) {}

  /**
   * Fill a Uint8Array with frequency-domain data (0-255 scaled to minDecibels..maxDecibels).
   * @param {Uint8Array} array
   */
  getByteFrequencyData(array) {}

  /**
   * Fill a Float32Array with time-domain (waveform) data (-1.0 to 1.0).
   * @param {Float32Array} array
   */
  getFloatTimeDomainData(array) {}

  /**
   * Fill a Uint8Array with time-domain data (0-255, 128 = zero crossing).
   * @param {Uint8Array} array
   */
  getByteTimeDomainData(array) {}

  /** @param {AudioNode} destination @returns {AudioNode} */ connect(destination) {}
  disconnect() {}
}


// -----------------------------------------------------------------------------
// AudioParam
// -----------------------------------------------------------------------------
// Wraps a single automatable parameter on a voice or filter.

class AudioParam {
  /** Current value. Setting this immediately updates the engine. */
  get value() {}
  set value(v) {}
}


// -----------------------------------------------------------------------------
// MediaStream / MediaStreamAudioSourceNode
// -----------------------------------------------------------------------------
// Obtained via `navigator.mediaDevices.getUserMedia({ audio: true })`.

class MediaStream {
  // Opaque handle representing an active microphone capture.
}

class MediaStreamAudioSourceNode {
  /**
   * Connect the mic source to an AnalyserNode (sets its source to mic).
   * @param {AnalyserNode} destination
   * @returns {AnalyserNode}
   */
  connect(destination) {}
  disconnect() {}
}


// -----------------------------------------------------------------------------
// VoiceAllocator
// -----------------------------------------------------------------------------
// Created via `ctx.createVoiceAllocator(maxVoices)`.
// Manages polyphonic voice allocation with steal policies.

class VoiceAllocator {

  /** Number of currently active voices (read-only). */
  get activeVoiceCount() {}

  /**
   * Trigger a note. Allocates or steals a voice.
   * @param {number} note - MIDI note number (0-127)
   * @param {number} velocity - 0.0 to 1.0
   * @param {number} [when=currentTime] - start time
   * @returns {number} voiceId
   */
  noteOn(note, velocity, when) {}

  /**
   * Release a note.
   * @param {number} note - MIDI note number
   * @param {number} [when=currentTime]
   */
  noteOff(note, when) {}

  /** Release all active notes. @param {number} [when=currentTime] */
  allNotesOff(when) {}

  /**
   * Set voice-stealing policy.
   * @param {string} policy - "oldest"|"quietest"|"samenote"|"none"
   */
  setStealPolicy(policy) {}

  /** @param {number} count */ setMaxVoices(count) {}

  /**
   * Register a callback invoked whenever a new voice is allocated.
   * Use this to configure the voice (waveform, envelope, wavetable, etc.).
   * @param {(voiceId: number, note: number, velocity: number) => void} callback
   */
  setVoiceSetup(callback) {}

  /**
   * Get the voice ID currently playing a given note, or -1.
   * @param {number} note
   * @returns {number} voiceId
   */
  voiceForNote(note) {}
}


// -----------------------------------------------------------------------------
// ModMatrix
// -----------------------------------------------------------------------------
// Obtained via `ctx.getModMatrix()`. Routes modulation sources to destinations.

class ModMatrix {

  /** Number of active routes (read-only). */
  get routeCount() {}

  // --- LFO Control ----------------------------------------------------------
  // Up to 4 LFOs (indices 0-3).

  /**
   * @param {number} lfoIndex - 0-3
   * @param {string} shape - "sine"|"triangle"|"square"|"sawup"|"sawdown"|"sampleandhold"
   */
  setLfoShape(lfoIndex, shape) {}

  /** @param {number} lfoIndex @param {number} hz */ setLfoRate(lfoIndex, hz) {}
  /** @param {number} lfoIndex @param {number} depth */ setLfoDepth(lfoIndex, depth) {}
  /** @param {number} lfoIndex @param {number} offset */ setLfoOffset(lfoIndex, offset) {}
  /** @param {number} lfoIndex @param {boolean} bipolar */ setLfoBipolar(lfoIndex, bipolar) {}
  /** @param {number} lfoIndex @param {boolean} sync */ setLfoSync(lfoIndex, sync) {}

  // --- Route Management -----------------------------------------------------

  /**
   * Add a modulation route.
   * @param {string} source - "lfo1"|"lfo2"|"lfo3"|"lfo4"|"envelope"|"velocity"|"keytracking"|"modwheel"|"aftertouch"
   * @param {string} dest - "pitch"|"gain"|"pan"|"filterfreq"|"filterq"|"pulsewidth"|"delaysend"
   * @param {number} amount
   * @returns {number} routeIndex
   */
  addRoute(source, dest, amount) {}

  /** @param {number} routeIndex */ removeRoute(routeIndex) {}
  /** @param {number} routeIndex @param {number} amount */ setRouteAmount(routeIndex, amount) {}
  /** @param {number} routeIndex @param {boolean} enabled */ setRouteEnabled(routeIndex, enabled) {}
  clearAllRoutes() {}

  // --- External Mod Sources -------------------------------------------------

  /** @param {number} value - 0.0 to 1.0 */ setModWheel(value) {}
  /** @param {number} value - 0.0 to 1.0 */ setAftertouch(value) {}
}


// -----------------------------------------------------------------------------
// MidiInput
// -----------------------------------------------------------------------------
// Created via `ctx.createMidiInput()`. Connects hardware MIDI controllers.

class MidiInput {

  /** Whether a MIDI port is currently open (read-only). */
  get isOpen() {}

  /**
   * List available MIDI input ports.
   * @returns {{ index: number, name: string }[]}
   */
  availablePorts() {}

  /**
   * Open a MIDI port by index.
   * @param {number} portIndex
   * @returns {boolean} success
   */
  open(portIndex) {}

  close() {}

  /**
   * Connect MIDI note on/off events directly to a VoiceAllocator.
   * @param {VoiceAllocator} allocator
   */
  connectToAllocator(allocator) {}

  /**
   * Register a callback for a specific MIDI CC number.
   * @param {number} cc - 0-127
   * @param {(channel: number, cc: number, value: number) => void} callback
   */
  onControlChange(cc, callback) {}

  /**
   * Register a callback for pitch bend messages.
   * @param {(channel: number, value: number) => void} callback - value is -8192 to 8191
   */
  onPitchBend(callback) {}

  /**
   * Register a catch-all callback that receives every parsed MIDI event before
   * type-specific dispatch (CC / pitch bend / note routing).
   * Use this for aftertouch, channel pressure, program change, or to log raw input.
   *
   * The event object has shape:
   *   {
   *     type: "noteon"|"noteoff"|"controlchange"|"pitchbend"
   *           |"programchange"|"aftertouch"|"channelpressure",
   *     channel: number,    // 0-15
   *     data1: number,      // note / cc / program / poly-key
   *     data2: number,      // velocity / cc value / pressure
   *     pitchBend: number,  // -8192..8191 (only valid when type === "pitchbend")
   *     timestamp: number,  // engine seconds
   *   }
   *
   * @param {(event: object) => void} callback
   */
  onRawEvent(callback) {}

  /** Poll and dispatch pending MIDI events. Call this each frame. */
  processEvents() {}
}


// -----------------------------------------------------------------------------
// Sequence
// -----------------------------------------------------------------------------
// Created via `ctx.createSequence(allocator)`. A timeline of note events.

class Sequence {

  /** Current BPM (read-only via property, set via setBPM). */
  get bpm() {}

  /** Whether the sequence is currently playing (read-only). */
  get playing() {}

  /** Whether the sequence is paused (read-only). */
  get paused() {}

  /** Whether looping is enabled (read-only). */
  get loopEnabled() {}

  /** Number of notes in the sequence (read-only). */
  get noteCount() {}

  /** Number of automation lanes (read-only). */
  get automationLaneCount() {}

  /** @param {number} bpm */ setBPM(bpm) {}
  /** @param {number} numerator @param {number} denominator */ setTimeSignature(numerator, denominator) {}

  /**
   * Add a note event.
   * @param {number} beat - start beat position
   * @param {number} note - MIDI note number
   * @param {number} velocity - 0.0 to 1.0
   * @param {number} duration - in beats
   */
  addNote(beat, note, velocity, duration) {}

  /** @param {number} index */ removeNote(index) {}
  clearNotes() {}

  /**
   * Read back a note event by index (notes are kept sorted by beat).
   * @param {number} index - 0 .. noteCount-1
   * @returns {{beat:number, note:number, velocity:number, duration:number}|null}
   */
  note(index) {}

  /** @param {number} [when=currentTime] */ play(when) {}
  stop() {}
  /** @param {number} [when=currentTime] */ pause(when) {}
  /** @param {number} [when=currentTime] */ resume(when) {}

  /** @param {boolean} enabled */ setLoopEnabled(enabled) {}
  /** @param {number} startBeat @param {number} endBeat */ setLoopRange(startBeat, endBeat) {}

  /**
   * Get the current beat position.
   * @param {number} [when=currentTime]
   * @returns {number} beat
   */
  currentBeat(when) {}

  /**
   * Advance the sequence (fires note-on/off and automation). Call each frame.
   * @param {number} [when=currentTime]
   */
  update(when) {}

  // --- Automation Lanes -----------------------------------------------------

  /**
   * Add an automation lane with a callback that receives interpolated values.
   * @param {(value: number) => void} callback - called during update() with the current value
   * @returns {number} laneIndex
   */
  addAutomationLane(callback) {}

  /** @param {number} laneIndex */ removeAutomationLane(laneIndex) {}
  clearAutomationLanes() {}

  /**
   * Add a breakpoint to an automation lane.
   * @param {number} laneIndex
   * @param {number} beat
   * @param {number} value
   */
  addAutomationPoint(laneIndex, beat, value) {}

  /** @param {number} laneIndex @param {number} pointIndex */ removeAutomationPoint(laneIndex, pointIndex) {}
  /** @param {number} laneIndex */ clearAutomationPoints(laneIndex) {}

  /**
   * Set interpolation mode for a lane.
   * @param {number} laneIndex
   * @param {string} mode - "linear"|"step"|"smooth"
   */
  setAutomationInterpMode(laneIndex, mode) {}

  // --- Automation read-back -------------------------------------------------

  /** @param {number} laneIndex @returns {number} number of breakpoints in the lane */
  automationPointCount(laneIndex) {}

  /**
   * Read back an automation breakpoint (points are kept sorted by beat).
   * @param {number} laneIndex
   * @param {number} pointIndex - 0 .. automationPointCount(laneIndex)-1
   * @returns {{beat:number, value:number}|null}
   */
  automationPoint(laneIndex, pointIndex) {}

  /** @param {number} laneIndex @returns {string|null} "linear"|"step"|"smooth" */
  automationInterpMode(laneIndex) {}
}


// -----------------------------------------------------------------------------
// AudioDestinationNode
// -----------------------------------------------------------------------------
// Represents the final audio output. Available as `ctx.destination`.
// Used as a connect() target only.

class AudioDestinationNode {}
