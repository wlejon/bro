/**
 * =============================================================================
 * bro.remote — host this screen for a remote viewer (broremote)
 * =============================================================================
 *
 * A page starts a broremote server; a viewer (bro's `<remoteview>`, below,
 * e.g. helmapps' remote app, on this machine or over ssh; or `broremote
 * probe` for scripted checks) attaches to it, sees what bro composites and drives
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
 *                            "default", which `bro.remote.connect({ ssh: HOST })`
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
 *                repeats,            the last picture encoded again with no new frame: sharpening a
 *                                    still screen, or the keyframe a viewer asked for
 *                windowWaits,        frames submitted while a viewer was at its ack window
 *                lanes,              input lanes joined
 *                audioLanes,         audio lanes joined
 *                audioUp, audioDown }  audio packets from viewers' mics / to viewers
 *       audio: {                      (while hosting)
 *           enabled, micAsDefault,    as configured
 *           status,                   what is wrong with this machine's audio
 *                                     now ('' when nothing): no audio system,
 *                                     or PipeWire restarting. A restarted
 *                                     PipeWire is reconnected to by itself,
 *                                     and each viewer's mic source and the
 *                                     desktop capture come back
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
 * =============================================================================
 * THE VIEWER SIDE: bro.remote.connect() and <remoteview>
 * =============================================================================
 *
 * The other end: a page shows another machine's broremote server (helm
 * --remote, `broremote serve-test`, another bro hosting) and drives it.
 * helmapps' remote app is built on it. Needs the "remote"
 * permission, like host().
 *
 *   bro.remote.connect(options?) -> session
 *       options.ssh          ssh destination ([user@]host): `ssh HOST
 *                            broremote proxy`; none: a socket on this machine
 *       options.socket       the server's socket name (default "default")
 *       options.sshCommand   the remote command instead of `broremote proxy`
 *       options.sshProgram   the ssh to run (default: Windows' own OpenSSH
 *                            where present, else ssh from PATH)
 *       options.pty          run the proxy on a remote pty (ssh -tt)
 *       options.inputLane    input on a connection of its own (default true)
 *       options.name         this viewer's name, as the server sees it ("bro")
 *       options.negotiate    tell the server which codecs decode here (true)
 *       options.audio        the audio lane (true); options.mic, options.playback
 *                            (both true), micMuted / playbackMuted (start muted),
 *                            micDevice / speakerDevice (part of a name),
 *                            micTone (Hz: a tone instead of the mic),
 *                            audioBufferMs (the playback jitter target, 20)
 *   session.id, session.target
 *   session.status() -> { state: 'connecting' | 'connected' | 'closed',
 *       message, failed, server, protocol ("1.5"), codec, width, height, fps,
 *       stream, decoder, hardware, inputLane, inputLaneError, micMuted,
 *       playbackMuted, cursor: { visible, x, y, shape, locked } | null }
 *   session.stats() -> { packets, decoded, failed, keyframeRequests,
 *       gpuPictures (decoded pictures that stayed on the GPU), bytes, seconds,
 *       fps, mbps, decodeMs, decodeMaxMs (since the last call),
 *       latency: { frames, rtt, age, maxAge, queue, encode, wait, net, dwait,
 *                  decode, present, kbytes } }   (ms, the mean per frame)
 *   session.audio() -> null, or the audio lane: { connected, closed, playback,
 *       mic, micNode, rttMs, micLatencyMs, playbackLatencyMs, notes,
 *       hostStatus (protocol 1.5: what is wrong with the host's audio now,
 *       e.g. its PipeWire restarting; '' when nothing; also in notes), ... }
 *   session.probe() -> bool    a latency probe (a press and release of
 *                              KEY_F13) against `serve-test --latency`; false
 *                              while one is open
 *   session.probes() -> { count, lost, presented: {mean, p50, p90, min, max},
 *       decoded: {...}, parts: { uplink, queue, encode, wait, net, dwait,
 *       decode, present, rtt } }   key press to the answering picture (ms)
 *   session.sendInput({ kind: 'key' | 'button' | 'motion' | 'relative' |
 *       'wheel', code, pressed, x, y, wheelX, wheelY })   (evdev codes,
 *       stream pixels; a <remoteview> sends the user's input itself)
 *   session.setMicMuted(b), session.setPlaybackMuted(b)
 *   session.close()           ends it; the 'state' event says 'closed'
 *   session.on('state' | 'config', fn) / off, onstate / onconfig
 *
 * <remoteview> (HTMLRemoteViewElement) shows a session: its own compositor
 * layer, the picture letterboxed (aspect kept) in the content box. Its
 * intrinsic size is the stream's (300x150 until there is one).
 *   view.session          the session shown (null: none). A session shows in
 *                         one view at a time; a closed one leaves the view.
 *   view.captured         keys go to the remote screen
 *   view.capture()        focus the view and capture; a press on it does too
 *   view.release()        give the keyboard back
 *   view.releaseChord     the chord that releases ("Ctrl+Alt+Escape"):
 *                         modifiers (Ctrl, Alt, Shift, Meta) and a key by its
 *                         KeyboardEvent.code (a bare letter or digit works)
 *   view.streamWidth / streamHeight
 *   view.stats() -> { path: 'gpu' | 'cpu' | 'none', zeroCopy, pictures,
 *       gpuPictures, uploads, unshown, presented, streamWidth, streamHeight,
 *       bridge, bridgeImports, bridgeConversions, bridgeError? }
 *   events: 'capture', 'release'
 *
 *   Pictures. On Windows the decoder (Media Foundation, the GPU's own) leaves
 *   each picture on the GPU; the D3D11 video processor converts it into a
 *   texture Vulkan shares (VK_KHR_external_memory_win32), and the compositor
 *   samples that: no CPU readback, no copy through system memory
 *   (stats().zeroCopy). When that cannot work (the decoder on another
 *   adapter than bro's) and elsewhere, pictures are decoded to the CPU and
 *   uploaded. Linux decode in hardware (VA-API) will hand over dmabufs,
 *   imported as client windows' buffers are.
 *   Input. While captured (and focused), every key goes to the remote screen
 *   before anything of bro's own sees it: no DOM key events, no hotkeys, no
 *   text input, and the window grabs the keyboard so Alt+Tab and the Windows
 *   key go too; only the release chord stays here. The pointer over the view
 *   goes to the remote screen in stream pixels (and to the page as usual,
 *   which may show controls on hover); the wheel only to the remote screen.
 *   While the remote screen's pointer is locked (a game; cursor.locked) the
 *   local pointer is hidden and held, and only its movement is sent. The
 *   pointer shape over the view is the remote screen's. Blur, or the window
 *   losing focus, lets go of every key and button held through the view.
 *   A window showing a remote screen is never slowed down while unfocused.
 *
 * TESTING: tests/remote (bro_remote_host_test) hosts headless and windowed
 * and attaches a viewer that checks the pixels and sends a key and a click;
 * then a second bro-headless views the first through a <remoteview>
 * (view_remote.js) and does the same through it.
 */

// ---------------------------------------------------------------------------
// Viewing: connect, show, and get the keyboard back.
// ---------------------------------------------------------------------------
function viewRemote(host) {
    const view = document.createElement('remoteview');
    view.style.cssText = 'position: absolute; inset: 0; width: 100%; height: 100%; display: block;';
    document.body.appendChild(view);
    const session = bro.remote.connect({ ssh: host, socket: 'helm' });
    view.session = session;
    session.onstate = () => {
        const s = session.status();
        if (s.state === 'connected') view.capture();
        if (s.state === 'closed') console.log('closed: ' + s.message);
    };
    view.addEventListener('release', () => console.log('keys are back; click the view to send them again'));
    return session;
}

// ---------------------------------------------------------------------------
// Host on the default socket, with hardware HEVC (else H.264) where the
// machine has it.
// ---------------------------------------------------------------------------
if (bro.remote.available) {
    const status = bro.remote.host({ codecs: ['hevc', 'h264', 'raw'] });
    console.log(`hosting on ${status.socketPath}`);
    // On another machine:  bro.remote.connect({ ssh: 'this-host' }) in a
    // <remoteview> (helmapps' remote app), or `broremote probe --ssh this-host`
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
