/**
 * =============================================================================
 * Video / VideoEncoder / GifEncoder / bro.media — Video Playback & Encoding
 * =============================================================================
 *
 * Media encoding and timeline analysis pipeline:
 *   - VideoEncoder: WebM/VP9 video + Opus audio encoding
 *   - GifEncoder: Animated GIF89a export with median-cut quantization
 *   - bro.media: Audio waveform peak and thumbnail strip extraction
 *
 * @example
 *   // Encode WebM video from canvas
 *   const enc = new VideoEncoder({
 *     path: 'output.webm',
 *     width: 640,
 *     height: 480,
 *     fps: 30,
 *     quality: 'good'
 *   });
 *   enc.addCanvasFrame(canvas);
 *   enc.finish();
 *
 * @example
 *   // Extract waveform peaks
 *   const peaks = bro.media.peaks('audio.wav', { buckets: 2048 });
 *   console.log('Sample rate:', peaks.sampleRate, 'Duration:', peaks.duration);
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * WebM/VP9 video and Opus audio encoder.
 */
class VideoEncoder {

  /**
   *  Initializes a new VideoEncoder instance with config options.
   *
   * @param {Object} config
   */
  constructor(config) {}

  /**
   *  Configured video frame width.
   * @readonly
   * @type {number}
   */
  width;

  /**
   *  Configured video frame height.
   * @readonly
   * @type {number}
   */
  height;

  /**
   *  Number of encoded frames written to file.
   * @readonly
   * @type {number}
   */
  framesWritten;

  /**
   *  Last encoder error message.
   * @readonly
   * @type {string}
   */
  lastError;

  /**
   *  Push an RGBA frame buffer to encode.
   *
   * @param {Uint8Array} pixels
   * @param {number} [stride]
   */
  addFrameRGBA(pixels, stride) {}

  /**
   *  Snapshot and encode a canvas element frame.
   *
   * @param {Object} canvas
   */
  addCanvasFrame(canvas) {}

  /**
   *  Snapshot and encode full composited viewport frame.
   */
  addViewportFrame() {}

  /**
   *  Push interleaved float32 PCM audio samples.
   *
   * @param {Float32Array} pcm
   */
  addAudioFramesPCM(pcm) {}

  /**
   *  Finalizes encoding and closes output file.
   */
  finish() {}

}

/**
 * Animated GIF89a encoder with per-frame 256-color palette quantization.
 */
class GifEncoder {

  /**
   *  Initializes a new GifEncoder instance with config options.
   *
   * @param {Object} config
   */
  constructor(config) {}

  /**
   *  Configured frame width.
   * @readonly
   * @type {number}
   */
  width;

  /**
   *  Configured frame height.
   * @readonly
   * @type {number}
   */
  height;

  /**
   *  Number of frames written.
   * @readonly
   * @type {number}
   */
  framesWritten;

  /**
   *  Last encoder error message.
   * @readonly
   * @type {string}
   */
  lastError;

  /**
   *  Push an RGBA frame buffer to encode.
   *
   * @param {Uint8Array} pixels
   * @param {number} [stride]
   */
  addFrameRGBA(pixels, stride) {}

  /**
   *  Snapshot and encode a canvas element frame.
   *
   * @param {Object} canvas
   */
  addCanvasFrame(canvas) {}

  /**
   *  Snapshot and encode full viewport frame.
   */
  addViewportFrame() {}

  /**
   *  Set delay for subsequent frame in centiseconds (1/100s).
   *
   * @param {number} delayCs
   */
  setNextFrameDelayCs(delayCs) {}

  /**
   *  Finalizes GIF encoding and writes trailer.
   */
  finish() {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * Media file analysis and timeline diagnostics namespace.
 */
/**
 *  Whether media analysis subsystem is available.
 * @readonly
 * @type {boolean}
 */
bro.media.available;

/**
 *  Extract waveform min/max/rms peaks from audio/video file.
 *
 * @param {string} path
 * @param {{buckets?: number, from?: number, to?: number}} [options] - `buckets` defaults to 2048; `from`/`to` in seconds (0 = start/end)
 * @returns {{sampleRate: number, channels: number, duration: number, from: number, to: number, buckets: number, min: Float32Array, max: Float32Array, rms: Float32Array}|null}
 */
bro.media.peaks = function(path, options) {};

/**
 *  Extract thumbnail strip image from video file.
 *
 * @param {string} path
 * @param {{count?: number, height?: number, from?: number, to?: number}} [options] - `count` defaults to 24, `height` to 72; `from`/`to` in seconds
 * @returns {{width: number, height: number, count: number, rotation: number, times: Float64Array|Array<number>, data: Uint8ClampedArray}|null}
 */
bro.media.thumbnails = function(path, options) {};

/**
 *  Read audio container metadata tags and duration without decoding audio samples.
 *
 * @param {string} path
 * @returns {{title: string, artist: string, album: string, albumArtist: string, track: number, trackTotal: number, disc: number, discTotal: number, year: number, genre: string, duration: number, sampleRate: number, channels: number, bitrate: number, codec: string, container: string, picture: {mime: string, bytes: Uint8Array}|null}|null}
 */
bro.media.tags = function(path) {};

/**
 *  Check whether this build/platform can decode an audio MIME type or file extension.
 *
 * @param {string} type
 * @returns {''|'maybe'|'probably'}
 */
bro.media.canDecode = function(type) {};


