/**
 * =============================================================================
 * bro.remote — host this screen for a remote viewer (broremote)
 * =============================================================================
 *
 * A page starts a broremote server; a viewer (`broremote-view`, on this
 * machine or over ssh) attaches to it, sees what bro composites and drives
 * it with its own keyboard and mouse. helm uses it for `helm --remote`.
 * The binding is broremote's own (broremote_api, ../broremote/src/api); bro
 * feeds it frames and input (src/bronze_host/host_remote.cpp).
 *
 * Build: BRO_WITH_REMOTE, on by default on Windows and Linux (broremote comes
 * from ../broremote or bro's pin). Off, `bro.remote` is the usual stub:
 * `available: false`, `reason`, and every call throws. Main realm only (not
 * in Workers). One server per process: a page reload keeps it running, and
 * the reloaded page sees it in status() and can stop it.
 *
 * WHAT A VIEWER GETS
 *   frames   Only while a viewer is attached; with none, hosting costs one
 *            branch a frame.
 *              bro --drm   every composited frame, as the KMS scanout buffer
 *                          itself (a dmabuf the encoder reads with no copy),
 *                          handed over as soon as it is rendered, before
 *                          its flip, so encoding never waits for a vblank.
 *                          While a viewer watches, a fullscreen client is
 *                          composited instead of scanned out directly, so the
 *                          stream sees it too.
 *              windowed    each presented frame read back to the CPU, at most
 *                          `fps` a second.
 *              headless    the frame composited on demand, on the virtual
 *                          clock (advanceTime), at most `fps` a second.
 *              bro-server  no frames.
 *            The stream's pixels are frame (device) pixels: the viewport
 *            times devicePixelRatio.
 *   input    The viewer's keys and buttons (evdev codes) and pointer (stream
 *            pixels) are routed exactly as local input: under DRM through
 *            the libinput path, so global hotkeys, the shell and client
 *            windows get them as they would the machine's own devices;
 *            elsewhere as ordinary key / mouse / wheel events (a printable
 *            key also delivers its text). Pages see trusted DOM events; there
 *            is nothing remote-specific to handle.
 *   cursor   The pointer's shape (CSS cursor name) and position. Under DRM
 *            the cursor is also drawn into the frame, as on screen.
 *   audio    (Linux with PipeWire; protocol 1.3, an audio lane of its own) Both
 *            ways, raw PCM:
 *              down  what this machine plays (the default output's monitor,
 *                    bro's own audio included) goes to the viewer's speakers.
 *              up    the viewer's microphone (on Windows the communications
 *                    mic, with Windows' echo cancellation where the device
 *                    has it) becomes a microphone here: a PipeWire source
 *                    named "broremote: <viewer's host> mic" that exists while
 *                    that viewer is attached. bro.mic, bro.stt, bro.wake and
 *                    any other program recording from it hear the viewer;
 *                    with micAsDefault it is the default source meanwhile,
 *                    so they hear it without being told, and the previous
 *                    default comes back when the viewer goes.
 *            Measured Windows to Linux over ssh on a LAN: about 25-30 ms mic
 *            to this machine, 35-42 ms this machine to the viewer's speakers.
 *            The viewer chooses (`--no-audio-lane`, `--no-mic`, mute keys);
 *            losing audio never touches frames or input.
 *
 * SECURITY: the socket is local and private to the user (a 0600 socket in a
 * 0700 directory under $XDG_RUNTIME_DIR, the peer's uid checked); a viewer on
 * another machine reaches it through `ssh HOST broremote proxy`, so ssh is
 * the authentication. Anyone who can attach controls the session.
 *
 * API
 *   bro.remote.available            true (false on the compiled-out stub)
 *   bro.remote.host(options?) -> status
 *       options.socket       socket name, 1-64 of [A-Za-z0-9._-] (default
 *                            "default", which `broremote-view --ssh HOST`
 *                            reaches with no other options)
 *       options.codecs       a name or an array in preference order: 'hevc',
 *                            'h264', 'av1', 'raw' (default ['hevc', 'h264']). Ones this
 *                            machine cannot encode are skipped; a viewer gets
 *                            the first one it can decode. Hardware encoders
 *                            are VA-API (Linux); elsewhere only 'raw'
 *                            (uncompressed, for a local or fast link).
 *       options.bitrateKbps  target bitrate (default 20000)
 *       options.fps          frame rate the encoder is set up for, and the cap
 *                            on CPU frames windowed and headless (default 60)
 *       options.name         the name viewers are told (default "bro")
 *       options.audio        offer viewers the audio lane (default true; it is
 *                            off anyway where there is no PipeWire)
 *       options.micAsDefault make each viewer's mic the default source while
 *                            it is attached (default false)
 *     Hosting already with the same options is a no-op; other options
 *     replace the server (attached viewers are disconnected). Throws a
 *     TypeError for a bad option, an Error when the server cannot start
 *     (e.g. "no configured codec can be encoded here", or another live
 *     server already owns the socket).
 *   bro.remote.stop() -> bool        whether a server was running
 *   bro.remote.status() -> {
 *       hosting, socket, socketPath, clients,
 *       codec, width, height,         the stream being sent (null / 0 before
 *                                     the first frame is encoded)
 *       bitrateKbps, fps, codecs,     (while hosting) the configuration
 *       stats: { submitted, encoded, keyframes, replaced, unwatched, failed, streams,
 *                windowWaits,        frames submitted while a viewer was at its ack window
 *                lanes,              input lanes joined
 *                audioLanes,         audio lanes joined
 *                audioUp, audioDown }  audio packets from viewers' mics / to viewers
 *       audio: {                      (while hosting)
 *           enabled, micAsDefault,    as configured
 *           viewers: [{ source,       the viewer's machine
 *                       playback, playbackMuted,   it hears this machine
 *                       mic, micMuted,             its mic is a source here
 *                       micNode,      the source's description, or null
 *                       micDefault,   it is the default source
 *                       micBufferMs, micUnderruns, micDroppedFrames }]
 *       }                             (micDroppedFrames grows while nothing
 *                                     records from the source: it is the
 *                                     bound on its buffer, not loss)
 *     }
 *   bro.remote.codecs() -> string[]  what this machine can encode ('raw' always)
 *   bro.remote.on(type, fn) / off(type, fn)
 *   bro.remote.addEventListener(type, fn) / removeEventListener(type, fn)
 *   bro.remote.onattach / ondetach    a viewer attached / left:
 *                                     fn({ type: 'attach' | 'detach', clients })
 *
 * TESTING: tests/remote (bro_remote_host_test) hosts headless and windowed
 * and attaches a viewer that checks the pixels and sends a key and a click.
 */

// ---------------------------------------------------------------------------
// Host on the default socket, with hardware HEVC (else H.264) where the
// machine has it.
// ---------------------------------------------------------------------------
if (bro.remote.available) {
    const status = bro.remote.host({ codecs: ['hevc', 'h264', 'raw'] });
    console.log(`hosting on ${status.socketPath}`);
    // On another machine:  broremote-view --ssh this-host
}

// ---------------------------------------------------------------------------
// Show who is watching.
// ---------------------------------------------------------------------------
bro.remote.on('attach', (e) => console.log(`a viewer attached (${e.clients} watching)`));
bro.remote.ondetach = (e) => {
    if (e.clients === 0) console.log('nobody is watching');
};

// ---------------------------------------------------------------------------
// A status line: the stream as encoded.
// ---------------------------------------------------------------------------
function remoteStatusText() {
    const s = bro.remote.status();
    if (!s.hosting) return 'not hosting';
    if (!s.codec) return `waiting for a viewer on "${s.socket}"`;
    return `${s.clients} watching: ${s.codec} ${s.width}x${s.height} @ ${s.bitrateKbps} kbps, ` +
           `${s.stats.encoded} frames`;
}

// ---------------------------------------------------------------------------
// A setting that turns hosting on and off (helm's ui/js/remote.js).
// ---------------------------------------------------------------------------
function setHosting(on) {
    if (on) bro.remote.host({ socket: 'desk', codecs: ['hevc', 'h264', 'raw'] });
    else bro.remote.stop();
}

// ---------------------------------------------------------------------------
// "Use my mic here to talk to that machine": the remote viewer's mic becomes
// the default source, so bro.mic (and stt / wake on top of it) hears the
// person at the viewer.
// ---------------------------------------------------------------------------
// The audio lane joins a moment after 'attach' (the viewer opens it once
// video is up), so read status().audio when it is needed, not in the event.
function hostWithRemoteMic() {
    bro.remote.host({ socket: 'desk', micAsDefault: true });
}
function remoteMicText() {
    const s = bro.remote.status();
    if (!s.hosting) return 'not hosting';
    const mics = s.audio.viewers.filter((v) => v.mic && !v.micMuted);
    return mics.length ? `listening to ${mics.map((v) => v.source).join(', ')}` : 'no remote mic';
}

// ---------------------------------------------------------------------------
// Headless: host, then let real time pass so a viewer can attach and be
// served (frames are taken on the virtual clock, input is delivered by
// advanceTime).
// ---------------------------------------------------------------------------
//   bro.remote.host({ socket: 'ci', codecs: 'raw', fps: 30 });
//   while (bro.remote.status().clients === 0) { advanceTime(16); wallSleep(4); }
