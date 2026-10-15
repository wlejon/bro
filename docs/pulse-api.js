/**
 * @file docs/pulse-api.js
 * @summary Documentation and examples for the `bro.pulse` JavaScript API.
 */

/**
 * `bro.pulse` provides per-application audio stream and sink routing over
 * PipeWire / PulseAudio (backed by native `bropulse`):
 * - Audio output sink enumeration and default sink selection (`getSinks`, `getDefaultSink`, `setDefaultSink`)
 * - Per-sink volume and mute control (`setSinkVolume`, `setSinkMuted`, `setSinkMute`)
 * - Active application playback stream enumeration (`getStreams`)
 * - Per-stream volume, mute, and live sink re-routing (`setStreamVolume`, `setStreamMuted`, `setStreamMute`, `moveStream`)
 * - Real-time sink and stream lifecycle events (`on`, `off`, `addEventListener`, `removeEventListener`)
 *
 * Privileged API: Mounted automatically in Bronze when `BRO_WITH_PULSE` is enabled and
 * the app is verified as a trusted shell app declaring `"shell": true` or
 * `"permissions": ["pulse", ...]` in its `bro.json`.
 */

// ============================================================================
// Type Definitions
// ============================================================================

/**
 * Audio output sink descriptor.
 * @typedef {Object} PulseSink
 * @property {number} id - Numeric sink identifier
 * @property {string} name - Internal sink node name
 * @property {string} description - Human-readable device description
 * @property {number} volume - Linear/normalized volume level (typically `0.0` to `1.0`, up to `1.5` for boost)
 * @property {boolean} isMuted - Whether the sink is muted
 * @property {boolean} isDefault - Whether this sink is the current default output
 * @property {number} channels - Channel count (e.g. `2` for stereo)
 * @property {'running'|'idle'|'suspended'|string} state - Current sink transport state
 */

/**
 * Per-application audio playback stream (sink-input) descriptor.
 * @typedef {Object} PulseStream
 * @property {number} id - Numeric stream identifier
 * @property {string} name - Stream title or media name
 * @property {number} pid - Client process ID (`0` if unknown)
 * @property {string} appId - Application identifier or binary name
 * @property {string} icon - Freedesktop icon name
 * @property {number} volume - Stream volume multiplier (`0.0` to `1.5`)
 * @property {boolean} isMuted - Whether the stream is muted
 * @property {number} sinkId - ID of the `PulseSink` this stream is currently routed to
 */

/**
 * Event object delivered for `'sinkRemoved'`, `'streamRemoved'`, and `'defaultSinkChanged'`.
 * Also implements `valueOf()` and `toString()` returning the numeric `id`.
 * @typedef {Object} PulseIdEvent
 * @property {'sinkRemoved'|'streamRemoved'|'defaultSinkChanged'} type - Event name
 * @property {number} id - Affected sink or stream ID
 * @property {number} [sinkId] - Present on `'sinkRemoved'` and `'defaultSinkChanged'`
 * @property {number} [streamId] - Present on `'streamRemoved'`
 */

// ============================================================================
// API Stubs
// ============================================================================

/**
 * Whether `bro.pulse` is compiled in and granted to this application.
 * @type {boolean}
 */
bro.pulse.available;

/**
 * Lists all available audio output sinks.
 *
 * @returns {PulseSink[]}
 */
bro.pulse.getSinks = function() {};

/**
 * Returns the current default audio output sink, or `null` if none is available.
 *
 * @returns {PulseSink|null}
 */
bro.pulse.getDefaultSink = function() {};

/**
 * Sets the default audio output sink by numeric ID.
 *
 * @param {number} sinkId
 * @returns {boolean} True if the default sink was changed
 */
bro.pulse.setDefaultSink = function(sinkId) {};

/**
 * Sets the volume of an audio output sink.
 *
 * @param {number} sinkId - Target sink ID
 * @param {number} volume - Volume level (`0.0` = silent, `1.0` = 100%, up to `1.5` = 150%)
 * @returns {boolean} True on success
 */
bro.pulse.setSinkVolume = function(sinkId, volume) {};

/**
 * Mutes or unmutes an audio output sink.
 *
 * @param {number} sinkId - Target sink ID
 * @param {boolean} isMuted - True to mute, false to unmute
 * @returns {boolean} True on success
 */
bro.pulse.setSinkMuted = function(sinkId, isMuted) {};

/**
 * Alias for `bro.pulse.setSinkMuted(sinkId, isMuted)`.
 *
 * @param {number} sinkId
 * @param {boolean} isMuted
 * @returns {boolean}
 */
bro.pulse.setSinkMute = function(sinkId, isMuted) {};

/**
 * Lists all active application audio playback streams.
 *
 * @returns {PulseStream[]}
 */
bro.pulse.getStreams = function() {};

/**
 * Sets the volume of an individual application audio stream.
 *
 * @param {number} streamId - Target stream ID
 * @param {number} volume - Volume level (`0.0` to `1.5`)
 * @returns {boolean} True on success
 */
bro.pulse.setStreamVolume = function(streamId, volume) {};

/**
 * Mutes or unmutes an individual application audio stream.
 *
 * @param {number} streamId - Target stream ID
 * @param {boolean} isMuted - True to mute, false to unmute
 * @returns {boolean} True on success
 */
bro.pulse.setStreamMuted = function(streamId, isMuted) {};

/**
 * Alias for `bro.pulse.setStreamMuted(streamId, isMuted)`.
 *
 * @param {number} streamId
 * @param {boolean} isMuted
 * @returns {boolean}
 */
bro.pulse.setStreamMute = function(streamId, isMuted) {};

/**
 * Moves an active application audio stream to a different output sink without
 * interrupting playback.
 *
 * @param {number} streamId - Stream ID to reroute
 * @param {number} sinkId - Destination sink ID
 * @returns {boolean} True if the stream was moved
 */
bro.pulse.moveStream = function(streamId, sinkId) {};

/**
 * Subscribes to audio router events. Callbacks are invoked as `(payload, id)`:
 * - `'sinkAdded'`: `(sink: PulseSink, sinkId: number)`
 * - `'sinkUpdated'`: `(sink: PulseSink, sinkId: number)`
 * - `'sinkRemoved'`: `(ev: PulseIdEvent, sinkId: number)`
 * - `'streamAdded'`: `(stream: PulseStream, streamId: number)`
 * - `'streamUpdated'`: `(stream: PulseStream, streamId: number)`
 * - `'streamRemoved'`: `(ev: PulseIdEvent, streamId: number)`
 * - `'defaultSinkChanged'`: `(ev: PulseIdEvent, sinkId: number)`
 * - `'change'` / `'*'`: fires on any of the events above
 *
 * Optional `bro.pulse.on<event>` handler properties (e.g. `bro.pulse.onsinkAdded`)
 * are also supported.
 *
 * @param {'sinkAdded'|'sinkUpdated'|'sinkRemoved'|'streamAdded'|'streamUpdated'|'streamRemoved'|'defaultSinkChanged'|'change'|'*'} event
 * @param {function((PulseSink|PulseStream|PulseIdEvent), number): void} callback
 * @returns {typeof bro.pulse} `bro.pulse` for chaining
 */
bro.pulse.on = function(event, callback) {};

/**
 * Unsubscribes a previously registered event listener.
 *
 * @param {string} event
 * @param {Function} callback
 * @returns {typeof bro.pulse}
 */
bro.pulse.off = function(event, callback) {};

/**
 * Alias for `bro.pulse.on(event, callback)`.
 * @param {string} event
 * @param {Function} callback
 * @returns {typeof bro.pulse}
 */
bro.pulse.addEventListener = function(event, callback) {};

/**
 * Alias for `bro.pulse.off(event, callback)`.
 * @param {string} event
 * @param {Function} callback
 * @returns {typeof bro.pulse}
 */
bro.pulse.removeEventListener = function(event, callback) {};

// ============================================================================
// Examples
// ============================================================================

if (bro.pulse.available) {
    // 1. Inspect output sinks and default sink
    const sinks = bro.pulse.getSinks();
    for (const sink of sinks) {
        console.log(`Sink #${sink.id}: ${sink.description} [vol=${Math.round(sink.volume * 100)}%, muted=${sink.isMuted}, default=${sink.isDefault}]`);
    }

    const defSink = bro.pulse.getDefaultSink();
    if (defSink) {
        bro.pulse.setSinkVolume(defSink.id, 0.8);
    }

    // 2. Inspect per-app audio streams and route them
    const streams = bro.pulse.getStreams();
    for (const s of streams) {
        console.log(`Stream #${s.id}: ${s.name} (${s.appId}, pid=${s.pid}) -> sink #${s.sinkId}`);
    }

    // 3. Subscribe to live audio graph changes
    bro.pulse.on('streamAdded', (stream) => {
        console.log(`New audio stream started: ${stream.name} (${stream.appId})`);
    });
    bro.pulse.on('defaultSinkChanged', (ev, sinkId) => {
        console.log(`Default output sink changed to #${sinkId}`);
    });
}
