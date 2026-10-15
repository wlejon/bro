/**
 * =============================================================================
 * yue.endless — the endless YuE2 stream, and bro.music.beat
 * =============================================================================
 *
 * A stream that never ends and goes where it is steered (YuE2Endless,
 * brosoundml yue2_endless.h). Where yue.stream (music-api.js) plays songs one
 * after another, this is one piece of music made 10 s at a time, the way
 * yue2-research's endless.js does it (FINDINGS 7-10), and steered by targets:
 *
 *   - one token history in chunks of 250 frames (10 s); each chunk sampled by
 *     yue.splice opened by the last `memory` tokens of the history (positions
 *     re-based: the model sees a clip, not a song), cot off with no score, the
 *     score with cot full;
 *   - each chunk rendered by yue.inpaint over a window of up to 475 solved
 *     frames kept on its left, the chunk, and 25 frames of the next chunk
 *     solved and dropped, so a chunk renders once the next one's first tokens
 *     exist;
 *   - the audio decoded just in time (decodeLeadSeconds ahead of the
 *     playhead) into one program, handed out emitLeadSeconds ahead of it,
 *     contiguous from sample 0, 48 kHz planar stereo.
 *
 * Unpaced with the same chunk schedule and seeds it is endless.js bit for bit
 * (tokens, latents, audio; yue2-research native/endless_stream.js checks
 * constant streams, memory-drop tours and scored measured-bridge tours).
 *
 * Targets. A target is what chunks are made under: style text, an optional
 * score (ABC: under it the tempo is exact, its Q: sets it), a lyric sheet,
 * and knobs (music-api.js YuE2Knobs: the semantic ones act on a chunk's
 * splice, texture and percussion on its render, energy at decode).
 *   - setTarget() now: from the first chunk whose tokens can still change.
 *     That is the first chunk not sampled yet, or the newest sampled one
 *     while it is only the previous chunk's lookahead and there is time to
 *     sample it again (`retarget`). A 'planned' event says which chunk and
 *     the program sample it is first heard at; stats.openChunk / openSample
 *     say it before you set anything. A style change is heard 6-16 s later.
 *   - setTarget({ ..., at }) / setPath([...]): timed targets in program
 *     seconds, each applied from the first chunk boundary at or after its
 *     time; a path drawn ahead is built on time.
 *   - knobs (setKnobs): energy from the audio not handed out yet (~1 s), the
 *     rest from the open chunk.
 *
 * Style changes, as the research found best (changeMode 'auto'):
 *   - no score: the change chunk is opened by only changeMemory tokens (12 =
 *     0.5 s) of the old music, so the new style takes (FINDINGS 10: the new
 *     style heard in 78% of its chunks, every change reached, where swapping
 *     the style text alone reaches 38%);
 *   - a score: the measured bridge. A fresh clip of the new style (no
 *     history, a beat longer than a chunk), a bridge chunk spliced from the
 *     old music toward it (4 candidates, the one the clip follows best), the
 *     bridge probe-rendered and its beat read with the spectral sensor at the
 *     score's period, the clip probe-rendered and cut by up to one beat so its
 *     beat lands on the bridge's, and the bridge's probe kept as its render.
 *     A change takes two chunks (the bridge, then the clip) and ~7 s of work,
 *     so it starts early; one set with no time left for that work is planned
 *     a chunk later (and the 'planned' event says so). Into the clip the beat
 *     is within 0.11 beats 15 times in 21 (median 0.05).
 *
 * Rolled lyrics (target.roll): each chunk's prefix carries lyricWindow lines
 * of the sheet (section tags kept, not counted), starting after the furthest
 * line the model reached in the chunks before it. Where the model is in the
 * lyrics is read off its own attention while the chunk is sampled: head
 * 14.10 (layer 14, query head 10) points at the lyric word being sung
 * (yue2-research FINDINGS 11; the right word on ~70% of frames, the right
 * line on ~84%, about 2 frames ahead of the voice). For every token the
 * stream reads that head's softmax mass on each word of the window; a line
 * is reached when the head pointed into 60% of its words, each for 2 frames
 * or more, on frames whose mass on the window's words is 0.2 or more. The
 * reading costs one head's dot products per token and is there as soon as
 * the chunk's tokens are, so the next window never waits on a render. The
 * head also walks through lines the model only hums, so those count as
 * passed. A 'lyrics' event per chunk, right after its 'sampled', carries
 * the reading (row.lyricWords, row.pointer: the word sung per frame,
 * row.pointerMass, row.linesReached): enough to show the words being sung,
 * 40 ms a frame, ahead of the audio. Without roll, the whole sheet in every
 * prefix loops the first verse and chorus.
 *
 * Branches: branch(target, path) builds an alternative continuation from the
 * open chunk beside the main line (at most maxBranches). commitBranch(id)
 * makes it the main line: seamlessly at its fork when nothing past the fork
 * has been handed out, else with a branchFadeSeconds crossfade at the first
 * chunk boundary it can make (a committing branch is built first; 3-9 s after
 * the request on a 4090). The other branches are dropped. discardBranch(id)
 * drops one. Each branch costs about what the main line does.
 *
 * Pacing. Paced (the default) the stream hands audio out only
 * emitLeadSeconds ahead of the playhead you report with setPlayhead()
 * (program samples played), so report what the player has really consumed.
 * The worker services the player inside every long operation (splice per
 * token, renders and decodes while the device runs them); stats.gaps
 * records any 0.2 s without a service. On a 4090 a chunk costs a splice of
 * ~1 s, a render of ~2 s and a decode of ~0.7 s: ~30% of the GPU per line.
 *
 * The stream holds the model and opts.vae (their `busy`) until it stops.
 *
 * @example
 *   const yue = bro.music.loadYuE2(modelDir);
 *   const vae = bro.music.loadVae(vaeDir, { decoderOnly: true });
 *   const st = yue.endless({
 *     vae,
 *     target: { style: 'folk, acoustic guitar, warm, male vocal', tag: 'folk' },
 *     onAudio: (a) => player.push(a),             // planar, a.sample0 contiguous
 *     onEvent: (e) => { if (e.kind === 'planned') ui.showEta(e.tag, e.sample / 48000); },
 *   });
 *   requestAnimationFrame(function tick() { st.setPlayhead(player.played()); requestAnimationFrame(tick); });
 *   // Later: a new style from the open chunk, and a path drawn ahead.
 *   st.setTarget({ style: 'synthwave, analog synthesizer, drum machine', tag: 'synth' });
 *   st.setPath([{ style: 'lo-fi hip hop, mellow', tag: 'lofi', at: 120 }]);
 *   // Try something without committing to it.
 *   const id = st.branch({ style: 'heavy metal, distorted guitar', tag: 'metal' });
 *   st.commitBranch(id);   // or st.discardBranch(id)
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * What chunks are made under. Fields left out of setTarget() keep their
 * current value; '' or null clears a string.
 * @typedef {Object} YuE2EndlessTarget
 * @property {string} [style] - "[Tags]" text
 * @property {string|null} [score] - ABC; none = cot off. Under a score its Q: is the tempo and changes bridge
 * @property {string} [lyrics] - The sheet (YuE2's format, [section] lines); '' = instrumental
 * @property {boolean} [roll=false] - Roll the sheet (see Rolled lyrics above)
 * @property {number} [lyricWindow=4] - Rolled: lines a chunk's prefix carries, 1..32
 * @property {YuE2Knobs} [knobs] - Clamped (music-api.js)
 * @property {string} [tag] - Your label, echoed in events, rows and stats
 * @property {number} [at] - Program seconds: on the path, from the first chunk boundary at or after it. Left out = now
 */

/**
 * yue.endless(opts). Durations are seconds, frames are latent frames (25 a
 * second; program sample = frame * 1920).
 * @typedef {Object} YuE2EndlessOptions
 * @property {YuE2Vae} vae - A loaded VAE (decoderOnly is enough)
 * @property {YuE2EndlessTarget} [target] - The first chunk's (at ignored)
 * @property {YuE2EndlessTarget[]} [path] - Timed targets ahead (each needs at)
 * @property {function(YuE2AudioChunk)} onAudio - Program audio, planar, plus sample0 (contiguous from 0)
 * @property {function(YuE2EndlessEvent)} [onEvent]
 * @property {function({cancelled: boolean, error: string})} [onDone]
 * @property {boolean} [paced=true] - false: as fast as it can, maxChunks of them (offline, tests)
 * @property {number} [maxChunks=0] - Unpaced: chunks to make (required); paced: 0 = endless
 * @property {number} [chunkFrames=250] - 25..1000
 * @property {number} [memory=250] - Tokens opening a chunk (10 s)
 * @property {string} [changeMode='auto'] - 'auto' (drop without a score, bridge under one) | 'drop' | 'bridge' | 'plain'
 * @property {number} [changeMemory=12] - Tokens opening a dropped change's chunk
 * @property {number} [bridgeCandidates=4] - Bridge chunks sampled, the best kept
 * @property {number} [bridgeScoreFrames=125] - Frames of the fresh clip each candidate is scored on
 * @property {number} [lookahead=25] - Frames of the next chunk a render solves and drops
 * @property {number} [narLeft=475] - Solved frames a render keeps on its left
 * @property {number} [odeSteps=32]
 * @property {number} [cfgScale] - The protocol default when left out
 * @property {number} [seed=1] - Chunk k's splice seed seed*1000+k, its render seed*1000+500+k (endless.js)
 * @property {number} [emitLeadSeconds=0.75] - Audio handed out ahead of the playhead
 * @property {number} [decodeLeadSeconds=4] - Audio decoded ahead of the playhead
 * @property {number} [aheadSeconds=14] - The least lead a chunk is sampled with (the measured costs can ask for more, ~15.7 s); less = targets heard sooner
 * @property {number} [bridgeLeadSeconds=7] - A bridge starts this much earlier
 * @property {boolean} [retarget=true] - A target set now may re-sample the newest chunk
 * @property {number} [maxBranches=2] - 0..3
 * @property {number} [branchFadeSeconds=1] - A commit past its fork crossfades this long (0.1..4)
 * @property {boolean} [chunkData=false] - Tokens on 'sampled' and latents on 'built' events
 */

/**
 * One chunk as made (endless.js's chunks.json row).
 * @typedef {Object} YuE2EndlessChunk
 * @property {number} index
 * @property {number} branch - 0 = the main line
 * @property {string} tag
 * @property {string} style
 * @property {boolean} scored
 * @property {boolean} change - The first chunk of a new style (a bridge's bridge chunk)
 * @property {string} how - '' | 'drop' | 'bridge' | 'fresh' | 'plain'
 * @property {number} memory - Tokens it was opened by
 * @property {boolean} retargeted - Sampled again for a target that came after it was
 * @property {number} spliceSeconds
 * @property {number} inpaintSeconds
 * @property {number} decodeSeconds
 * @property {number} narWindow - Frames its render solved
 * @property {boolean} probeRender - Rendered by its bridge's probe (kept)
 * @property {number} latentNorm - Mean frame norm (the corpus: 6.5..9.5; the opening chunk runs 9-10)
 * @property {boolean} rendered
 * @property {Float32Array} [bridgeScores] - Bridge: each candidate's NLL of the fresh clip's opening
 * @property {{P: number, streamPhase: number, freshPhase: number, cut: number, seconds: number}} [phaseFit] - Fresh clip: the beat fit (P in frames, phases in beats), the frames cut, the measurement's cost
 * @property {number} [trim] - Fresh clip: = phaseFit.cut
 * @property {string} lyrics - The sheet text its prefix carried
 * @property {number[]} [lyricLines] - Rolled: the window [from, to) of sung lines
 * @property {string[]} [lyricWords] - Rolled: the window's words in prompt order (lower case, runs of [a-z'])
 * @property {Int32Array} [lyricWordLines] - Rolled: each word's sheet line (counting sung lines, as lyricLines)
 * @property {Int32Array} [pointer] - Rolled: per frame (40 ms), the index into lyricWords of the word head 14.10 attends to most
 * @property {Float32Array} [pointerMass] - Rolled: per frame, the head's attention mass on all the window's words (0..1; under ~0.2 it is not on the lyrics)
 * @property {number[]} [linesReached] - Rolled: the sheet lines the head reached in this chunk
 * @property {YuE2Knobs} knobs
 */

/**
 * An event (onEvent). kind:
 *   'planned'      a target placed: chunk = where it applies, sample = where it is first heard
 *                  (a later 'planned' for the same tag moves it)
 *   'sampled'      a chunk's tokens exist (row; tokens with chunkData)
 *   'built'        a chunk is rendered (row; latents + frame0 with chunkData), value = its latent norm
 *   'styleChange'  a change made at chunk: text = 'drop' | 'bridge' | 'plain'
 *   'phaseFit'     a bridge's beat fit (row of the fresh clip), value = the cut in frames
 *   'lyrics'       rolled lyrics: the lyric head's reading of a chunk, right after its 'sampled'
 *                  (row.lyricWords / pointer / pointerMass / linesReached); value = the furthest
 *                  sheet line reached so far
 *   'breakWarning' a chunk's latent norm left 6.5..9.5 (value)
 *   'chunkStart'   a chunk's first sample handed out (sample)
 *   'branch'       a branch opened: branch = its id, chunk = its fork
 *   'committed'    branch made the main line: chunk = the boundary, sample = where it is heard alone,
 *                  value = the fade (0 = seamless)
 *   'discarded'    a branch dropped: text = why
 *   'knobs'        setKnobs: text = which moved ('energy/texture'); energy from sample, the rest from chunk
 *   'error'        text = the message (the stream stops)
 *   'done'         unpaced: maxChunks played out
 * @typedef {Object} YuE2EndlessEvent
 * @property {string} kind
 * @property {number} chunk
 * @property {number} branch
 * @property {string} tag
 * @property {number} sample - Program sample
 * @property {number} seconds - 'planned': sample / 48000; 'sampled' / 'built': the work's seconds
 * @property {number} value
 * @property {string} text
 * @property {YuE2EndlessChunk} [row]
 * @property {Int32Array} [tokens] - 'sampled' with chunkData: the chunk's 250 codec ids
 * @property {Float32Array} [latents] - 'built' with chunkData: frames x 64
 * @property {number} [frame0] - 'built': the first frame of latents
 */

/**
 * stream.stats (a snapshot).
 * @typedef {Object} YuE2EndlessStats
 * @property {number} playhead - Program samples
 * @property {number} emitted
 * @property {number} decoded
 * @property {number} playing - The chunk at the playhead
 * @property {number} sampled - Main line: chunks with tokens
 * @property {number} rendered - Main line: chunks with latents
 * @property {number} committed - Chunks whose tokens can no longer change
 * @property {number} openChunk - Where a target set now would apply
 * @property {number} openSample - ... and the program sample it would first be heard at
 * @property {number} aheadSeconds - Audio decoded ahead of the playhead
 * @property {YuE2EndlessTarget} target - The newest chunk's
 * @property {number} path - Timed targets not applied yet
 * @property {YuE2EndlessChunk[]} chunks - The main line from the playing chunk on
 * @property {{id: number, fork: number, sampled: number, rendered: number, tag: string, committing: boolean}[]} branches
 * @property {{seconds: number, what: string, chunk: number, emitted: number, lead: number}[]} gaps - The last 16 of 0.2 s or more without a service
 * @property {number} worstGapSeconds
 * @property {Object<string, number>} work - The worker's seconds per kind of work
 * @property {{splice: number, render: number, decode: number}} costSeconds - Per chunk, running means
 */

/**
 * bro.music.beat's result. A comb fitted to the onset envelope (100 frames
 * a second) at the period: the beats fall at firstBeat + n * period.
 * @typedef {Object} YuE2BeatFit
 * @property {number} phase - In beats, [0, 1), of the first beat (q/64 steps)
 * @property {number} period - Seconds (the one given, or the best within +-3% with searchPeriod)
 * @property {number} periodFrames - In envelope frames
 * @property {number} firstBeat - Seconds: phase * period. The spectral sensor reads ~0.04 s early (below)
 * @property {number} strength - The comb's mean envelope value at the beats
 * @property {number} contrast - strength over the envelope's mean: 1 = no beat at this period, a click track 20-40
 * @property {number} beats - Beats the comb summed
 * @property {Float32Array} [envelope] - With opts.envelope
 */

// ── Classes ──────────────────────────────────────────────────────────────────

/**
 * A running endless stream (yue.endless). Every control returns at once;
 * the worker applies it between steps.
 */
class YuE2Endless {
  /** Program samples the player has played; paces the stream. @param {number} samples */
  setPlayhead(samples) {}
  /**
   * Merge into the current target and apply it: now (from the open chunk;
   * replaces the path) or, with `at`, on the path (replacing its entries at
   * or after that time). A 'planned' event says where it is heard.
   * @param {YuE2EndlessTarget} target
   * @returns {YuE2EndlessTarget} the merged target
   */
  setTarget(target) {}
  /** The timed targets ahead (each with at), replacing them. @param {YuE2EndlessTarget[]} path */
  setPath(path) {}
  /**
   * Merge knobs into the current target's: energy from the audio not handed
   * out yet, the rest from the open chunk. A 'knobs' event follows.
   * @param {YuE2Knobs} knobs
   * @returns {YuE2Knobs} the knobs now, clamped
   */
  setKnobs(knobs) {}
  /** The score now (null: none), the rest of the target kept. @param {string|null} abc */
  setScore(abc) {}
  /**
   * An alternative continuation from the open chunk, under `target` merged
   * into the current one, then `path`. A 'branch' event follows.
   * @param {YuE2EndlessTarget} target
   * @param {YuE2EndlessTarget[]} [path]
   * @returns {number} its id (from 1, in call order)
   */
  branch(target, path) {}
  /** Make branch `id` the main line (see Branches above); 'committed' follows. @param {number} id */
  commitBranch(id) {}
  /** Drop branch `id`; 'discarded' follows. @param {number} id */
  discardBranch(id) {}
  /** End the stream; onDone follows. */
  stop() {}
  /** @readonly @type {YuE2EndlessStats} */ stats;
  /** The current target (what setTarget merges into). @readonly @type {YuE2EndlessTarget} */ target;
  /** @readonly @type {boolean} */ running;
  /** Each knob's clamp (bro.music.knobRanges). @readonly @type {Object<string, {min: number, max: number}>} */ knobRanges;
}

/**
 * On a loaded YuE2 (music-api.js): the endless stream. Holds the model and
 * opts.vae until it stops. TypeError on bad options (an unpaced stream
 * without maxChunks, ...), Error when one of them is busy.
 * @param {YuE2EndlessOptions} opts
 * @returns {YuE2Endless}
 */
YuE2.prototype.endless = function(opts) {};

// ── The beat sensor ──────────────────────────────────────────────────────────

/**
 * The beat of 48 kHz audio at a known period: an onset envelope at 100
 * frames a second and a comb fitted to it (brosoundml beat.h; what the
 * measured bridge reads, ported bit for bit from endless.js). Sensors:
 *   'spectral' (default) spectral flux over 48 log bands of a 2048-point FFT
 *              at 24 kHz: agrees with librosa's onset envelope within 0.1
 *              beats in 90% of windows of YuE2 music. It sees an onset coming
 *              as it enters its 85 ms window, so its beats read ~0.04 s early;
 *              phases from one sensor compare exactly.
 *   'energy'   log energy of the signal and of its first difference per 10 ms,
 *              differenced and rectified: on time, less robust on music (82%).
 * CPU, synchronous: ~25 ms for 10 s. Also bro.ear.beat.
 * @param {Float32Array|{samples: Float32Array, channels: number, sampleRate: number}} audio - Mono samples, or a
 *   1- or 2-channel planar buffer (an onAudio chunk, a decode) at 48 kHz
 * @param {{period?: number, bpm?: number, sensor?: string, searchPeriod?: boolean, envelope?: boolean}} opts -
 *   period (seconds) or bpm required; searchPeriod: the best period within +-3%
 * @returns {YuE2BeatFit}
 * @example
 *   const fit = bro.music.beat(decoded, { bpm: 90 });
 *   console.log('beat at', fit.firstBeat + 0.04, 's, every', fit.period, 's; contrast', fit.contrast);
 */
bro.music.beat = function(audio, opts) {};
