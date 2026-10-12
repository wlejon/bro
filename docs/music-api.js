/**
 * =============================================================================
 * bro.music — YuE2 song generation and the YuE2 VAE
 * =============================================================================
 *
 * YuE2-3B (m-a-p) writes a song from lyrics and a style. It is one
 * AR–NAR Mixture-of-Transformers backbone (Qwen3-shaped: 2048 hidden,
 * 28 layers) run in three stages, then a VAE:
 *
 *   1. plan        AR sampling of an ABC score (cot 'full' / 'melody';
 *                  skipped for cot 'off' or when opts.abc is supplied)
 *   2. semantic    AR sampling of codec tokens (25 per second), with CFG
 *   3. synthesize  NAR flow matching: a midpoint ODE (odeSteps) turns noise
 *                  into 64-dim latents, 25 frames per second
 *   4. decode      the YuE2 VAE: latents -> 48 kHz stereo
 *
 * generate() runs all of it. plan() / semantic() / synthesize() are the same
 * stages one at a time, so an app can edit the ABC, re-roll the semantic
 * tokens with another seed, or re-synthesize with other ODE settings.
 *
 * Pass a VAE as opts.vae and the job decodes as it goes: latent frames are
 * decoded in tiles as soon as each tile's right-hand context exists, and
 * onAudio receives each chunk, so playback can start before the song is
 * done. The tiles are exact (each carries vae.requiredHalo() frames of
 * context on either side and is cropped to its core), so the chunks put end
 * to end equal a one-pass decode.
 *
 * In the default protocol mode (narChunkFrames 0) the NAR synthesizes the
 * whole song in one bidirectional chunk after the last semantic token, so
 * the first audio arrives after the semantic stage. narChunkFrames > 0
 * synthesizes windows as their tokens arrive (lower latency, a quality
 * trade-off).
 *
 * Weights: the YuE2-3B folder (config.json, model.safetensors, qwen.tiktoken;
 * BF16, CC BY-NC 4.0) and the YuE2-Vae folder (config.json,
 * model.safetensors; FP32). Both load on the GPU by default. Every model runs
 * one op at a time (`busy`); a generate() with opts.vae holds both.
 *
 * Steering and capture work on the residual stream after a decoder layer,
 * on the AR path (plan / semantic) or the NAR path (synthesis): opts.steer
 * adds vectors, and opts.capture returns hidden states in result.hidden. That
 * is how musical attributes are located inside the model and pushed around.
 *
 * Audio is planar: samples holds channel 0's `frames` samples, then
 * channel 1's.
 *
 * Streaming playback and the radio: yue.stream(opts) plays songs as they are
 * built and steers them live (YuE2Stream). One worker interleaves each
 * song's stages:
 *   - a semantic session sampled a few tokens at a time, with the same
 *     sampler as generate(), so a seed gives the same tokens;
 *   - NAR windows of 250 then 500 frames, each solved over 25 inpainted
 *     frames on the left and 25 of lookahead;
 *   - a just-in-time VAE decode about 6 s ahead of the playhead.
 * Quality matches the whole-song render (latent cosine about 0.88, mel L1
 * within the render's seed-to-seed spread, no flux step at the seams). The
 * first audio comes about 2 s after the plan; with cot 'full' that is 4-7 s
 * from the request, depending on the plan's length.
 *
 * The stream hands out audio only emitLeadSeconds (0.75) ahead of the
 * playhead the player reports with setPlayhead(), so a knob can still
 * change what is not handed out. Audio is one program timeline from sample
 * 0, crossfades mixed in, and onAudio's chunks are contiguous: push them to
 * an append-only stream (ctx.createStream). Songs crossfade at the outro
 * (equal power, 2-4 s).
 *
 * Songs ahead: while one plays, the stream builds the songs queued after it,
 * one at a time, up to aheadSongs of them (1 by default; a radio keeps 3 or
 * so). Each is built whole: plan, tokens, latents, and its audio decoded, so
 * a built song waits as finished audio in host memory (about 66 MB for a
 * 170 s song) and its device memory is released; the GPU holds one build
 * beside the playing song however many wait. skip() to a built song is the
 * skip fade alone. Built ahead, a song is solved in one NAR chunk once its
 * tokens are done (when that fits before it is needed), so it is exactly the
 * whole-song render; only the first song, and one a skip is waiting for, pay
 * the windows' small quality cost. A 'want' event asks for songs whenever
 * fewer than aheadSongs wait after the playing one (value = how many), so
 * the app can supply fresh requests; with radio on (the default), a want left
 * unanswered for wantGraceSeconds is filled from the last request with the
 * next seed. drop(song) takes a queued song out (built or not), setPlan(song,
 * abc) rebuilds one from an edited score.
 *
 * Previews (previewSeconds, e.g. 30): each song is built as its opening
 * only, about 11 s of GPU on a 4090 (the plan is the whole song's; the
 * semantic stage stops at the preview plus a second of lookahead). A preview
 * nobody chooses plays that long and crossfades into the next built one,
 * like scanning stations. makeFull() continues the playing preview into the
 * whole song, seamlessly. Its semantic stage resumes after the preview's
 * tokens, and its last 2 s not yet heard are re-solved with the
 * continuation as their lookahead, inpainted against what precedes. Pressed
 * 15 s into a 30 s preview, the continuation's first audio past the preview
 * is ready about 2 s later, 13 s before the playhead gets there, with no
 * step at the join (brosoundml test_yue2_stream_preview). The playing song's
 * continuation builds before anything else; then the previews ahead.
 * autoFull (or setAutoFull) makes every song full as it goes on air: the
 * classic radio, with a skip still instant to a built preview.
 *
 * Steering (setKnobs, each knob clamped below where the music broke in
 * yue2-research):
 *   - energy: a latent offset, heard within the emit lead;
 *   - texture and percussion: NAR steers, re-solved from about 1.5 s ahead;
 *   - the semantic steers (arrangement and the style axes in knobRanges):
 *     the song continues from about 2.5 s ahead under them.
 * interrupt(+1) is "make it energetic now", in two stages:
 *   1. a latent offset, audible in about 0.75 s;
 *   2. an arrangement-steered continuation, audible in about 2.5 s, joined
 *      at a window edge; then the offset ramps back. The arrangement it
 *      reached stays as the knob.
 * Knobs carry into every song built after the change: a song being built or
 * not started takes the change from its start. A song already built when a
 * knob moves keeps what it was built with while it waits (so a skip to it
 * stays instant), and catches up when it plays: energy from its first sample
 * (re-decoded just before it starts), texture / percussion from about 1.5 s
 * in, the semantic steers from about 2.5 s in, as for the playing song. Each
 * song's stats row shows the knobs it carries. Interrupts act on the playing
 * song only.
 *
 * A stream holds the model and the VAE (`busy`) until it ends. stop() ends
 * it, and onDone fires.
 *
 * @example
 *   // A whole song, played as it is decoded.
 *   const yue = bro.music.loadYuE2(bro.models.find('YuE2-3B'));
 *   const vae = bro.music.loadVae(bro.models.find('yue2-vae'));
 *   const ctx = new AudioContext({ sampleRate: 48000 });
 *   let at = 0;
 *   yue.generate({
 *     style: 'indie folk, warm acoustic guitar, female vocal, 92 bpm',
 *     lyrics: '[verse]\nMorning light on the river\n...\n[chorus]\n...',
 *     vae,
 *     onAbc: (text) => abcView.textContent += text,
 *     onProgress: (p) => bar.value = p.done / p.total,
 *     onAudio: (c) => {
 *       const buf = ctx.createBuffer(2, c.frames, c.sampleRate);
 *       buf.copyToChannel(c.samples.subarray(0, c.frames), 0);
 *       buf.copyToChannel(c.samples.subarray(c.frames), 1);
 *       const src = ctx.createBufferSource();
 *       src.buffer = buf;
 *       src.connect(ctx.destination);
 *       at = Math.max(at, ctx.currentTime + 0.05);
 *       src.start(at);
 *       at += buf.duration;
 *     },
 *     onDone: (r, info) => console.log(info.cancelled ? 'stopped' : 'done', r.timing),
 *   });
 *
 * @example
 *   // Stage by stage: plan, edit the score, then the rest.
 *   yue.plan({ style, lyrics, onDone: (r) => {
 *     const plan = r.plan;
 *     plan.abc = editAbc(plan.abc);   // or pass opts.abc to skip planning
 *     yue.semantic(plan, { style, lyrics, abc: plan.abc, seed: 7, onDone: (s) =>
 *       yue.synthesize(s.plan, s.semantic, { odeSteps: 16, vae, onAudio: play }) });
 *   }});
 *
 * @example
 *   // Re-decode stored latents with streaming playback.
 *   vae.decode(latents, { coreFrames: 25, onChunk: play, onDone: () => {} });
 *
 * @example
 *   // A radio, steered live.
 *   const ctx = new AudioContext({ sampleRate: 48000 });   // the stream's rate
 *   const pb = ctx.createStream(2, 4 * 48000);             // append-only (audio-engine-api.js)
 *   const st = yue.stream({
 *     vae,
 *     songs: [{ style: 'synthpop, female vocal, 118 bpm', lyrics, seed: 1, tag: 'first' }],
 *     onAudio: (c) => {                                    // planar -> interleaved
 *       const il = new Float32Array(c.frames * 2);
 *       for (let i = 0; i < c.frames; i++) { il[2 * i] = c.samples[i]; il[2 * i + 1] = c.samples[c.frames + i]; }
 *       ctx.pushStreamSamples(pb, il);                     // 0.75 s ahead: fits the ring
 *     },
 *     onEvent: (e) => {
 *       if (e.kind === 'plan') planView.textContent = e.text;          // edit, then st.setNextPlan(abc)
 *       if (e.kind === 'breakWarning') meter.classList.add('warn');
 *     },
 *   });
 *   setInterval(() => st.setPlayhead(ctx.getStreamStats(pb).playedFrames), 50);
 *   energeticButton.onclick = () => st.interrupt(1);
 *   textureSlider.oninput = () => st.setKnobs({ texture: +textureSlider.value });
 *   skipButton.onclick = () => st.skip();
 *
 * @example
 *   // Three songs kept built ahead, each a fresh request from the app.
 *   const st = yue.stream({
 *     vae, aheadSongs: 3, onAudio: push,
 *     onEvent: (e) => {
 *       if (e.kind === 'want') for (let i = 0; i < e.value; i++) st.enqueue(nextConcept());
 *       if (e.kind === 'built') readyBadge.textContent = st.stats.readyAhead + ' ready';
 *     },
 *   });                                                    // the first want asks for 4
 *   skipButton.onclick = () => st.skip();                  // instant once one is built
 *   restyleButton.onclick = () => {                        // discard what waits, rebuild
 *     for (const s of st.stats.songs) if (!s.onAir) st.drop(s.index);
 *   };
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * One AR stage's sampling. Defaults differ per stage: the semantic stage's are
 * shown; abcSampling defaults to { temperature 0.7, topP 0.9, topK 30,
 * repetitionPenalty 1.005, penaltyWindow 100, minTokens 32, maxTokens 4096 }.
 * @typedef {Object} YuE2Sampling
 * @property {number} [temperature=1] - 0 = greedy
 * @property {number} [topP=0.95]
 * @property {number} [topK=100]
 * @property {number} [repetitionPenalty=1.2] - Windowed and sign-aware (a negative logit is multiplied, a positive one divided)
 * @property {number} [penaltyWindow=50] - How many recent tokens the penalty sees (1..100)
 * @property {number} [minTokens=200] - The end token is masked before this many
 * @property {number} [maxTokens=9000] - The stage stops here (result.semanticTruncated / plan.truncated)
 */

/**
 * A residual-stream steering vector.
 * @typedef {Object} YuE2Steer
 * @property {number} layer - Decoder layer 0..27; the delta is added to its output
 * @property {Float32Array} delta - 2048 floats, or rows * 2048 with perPosition
 * @property {boolean} [nar=false] - false: the AR path (plan / semantic); true: the NAR path (synthesis)
 * @property {number} [scale=1]
 * @property {boolean} [perPosition=false] - Row r is added at position pos0 + r (AR: token position in the stage's sequence; NAR: latent frame)
 * @property {number} [pos0=0]
 * @property {boolean} [plan=true] - AR: apply during the plan stage
 * @property {boolean} [semantic=true] - AR: apply during the semantic stage
 */

/**
 * Which hidden states to capture (returned in result.hidden).
 * @typedef {Object} YuE2Capture
 * @property {number[]} layers - Layers whose output is captured (AR and NAR)
 * @property {boolean} [plan=true] - Capture during the plan stage
 * @property {boolean} [semantic=true] - Capture during the semantic stage (the conditional branch)
 * @property {number[]} [narEvals] - NAR velocity evaluations to capture, counted per chunk from 0 (two per ODE step: step s is evals 2s and 2s+1); empty = none
 */

/**
 * Options for generate / plan / semantic / synthesize.
 * @typedef {Object} YuE2Options
 * @property {string} [style] - The "[Tags]" text: genre, instruments, voice, tempo...
 * @property {string} [lyrics] - The "[Lyrics]" text, with section tags like [verse] / [chorus]
 * @property {'full'|'melody'|'off'} [cot='full'] - What the plan stage writes: a full ABC score, a melody only, or nothing
 * @property {string} [abc] - Use this ABC score and skip planning
 * @property {number|bigint} [seed=831001] - Each AR stage reseeds with it; the NAR noise too
 * @property {number} [cfgScale] - Semantic CFG scale; below 0 = the protocol default (1.0, or 1.01 for cot 'off')
 * @property {YuE2Sampling} [abcSampling]
 * @property {YuE2Sampling} [semanticSampling]
 * @property {number} [odeSteps=32] - Midpoint ODE steps per NAR chunk
 * @property {number} [narChunkFrames=0] - 0 = the protocol's chunks (whole song); > 0 = streaming windows of this many frames
 * @property {boolean} [runSemantic=true] - generate(): false stops after the plan
 * @property {boolean} [runSynthesize=true] - generate(): false stops after the semantic tokens
 * @property {boolean} [reusePrefixKv=true] - generate(): reuse the semantic stage's KV cache for synthesis instead of prefilling again
 * @property {number} [abcDraftTokens=5] - Speculative ABC planning: 1..7 tokens drafted per forward by prompt lookup
 *   (the score repeats itself) and checked with standard speculative acceptance, so the plan's distribution is
 *   exactly that of no drafting; a given seed draws a different plan than at 0 (0 = off, the one-token path; greedy
 *   gives the same plan either way). The default 5: ~2.3-2.6 tokens per forward, the plan ~200 -> ~400 tok/s in BF16 (~540 with arWeights 'int8'), and
 *   cot 'full' time to first audio streaming 10.1 s -> 6.7 s (5.0 s with int8) on a 4090. Ignored with cfgScale
 *   (the plan has no CFG). timing.planForwards counts the forwards.
 * @property {YuE2Steer[]} [steer]
 * @property {YuE2Capture} [capture]
 * @property {YuE2Vae} [vae] - Decode the latents as they arrive (onAudio, result.audio)
 * @property {number} [decodeCoreFrames=50] - With vae: the largest decoded tile (smaller = more halo recompute)
 * @property {number} [decodeFirstFrames=8] - With vae: the first tile. Tiles grow from it: the first is decoded as soon
 *   as this many frames have their right context (halo frames past them), each later one waits until it can take twice
 *   the previous tile, up to decodeCoreFrames. Streamed in 50-frame NAR windows, the first window gives a 38-frame
 *   tile at once (~60 ms on a 4090), so audio starts right after the first latents. Output is exact for any tiling.
 * @property {number} [decodeHaloFrames] - With vae: context frames per side (at least vae.requiredHalo())
 * @property {function(string, Int32Array)} [onAbc] - ABC text as it is sampled, batched per tick, with its token ids (never split inside a UTF-8 character)
 * @property {function(Int32Array)} [onSemantic] - Codec ids (0..32767) as they are sampled, batched per tick
 * @property {function({frame0: number, frames: number, latents: Float32Array})} [onLatents] - Final latent frames, contiguous from 0, frame-major (frames * 64)
 * @property {function(YuE2AudioChunk)} [onAudio] - With vae: each decoded chunk, contiguous from sample 0
 * @property {function({stage: string, done: number, total: number})} [onProgress] - Tokens so far / budget per AR stage, ODE steps done / total for 'synthesize'
 * @property {function(YuE2Result, {cancelled: boolean, error: string})} [onDone] - Exactly once, last
 */

/**
 * The plan stage's output; pass it back to semantic() / synthesize().
 * @typedef {Object} YuE2Plan
 * @property {string} abc - The ABC score ('' for cot 'off')
 * @property {Int32Array} abcIds - Its token ids
 * @property {Int32Array} prefix - The token prefix the semantic stage continues
 * @property {boolean} truncated - Planning hit maxTokens before the score ended
 */

/**
 * @typedef {Object} YuE2Timing
 * @property {number} planSeconds
 * @property {number} semanticSeconds
 * @property {number} synthSeconds
 * @property {number} planTtft - Seconds to the first plan token after prefill
 * @property {number} semanticTtft
 * @property {number} planTokens
 * @property {number} semanticTokens
 * @property {number} planTps - Tokens per second
 * @property {number} planForwards - AR forwards the plan took; planTokens / planForwards is the tokens each
 *   forward accepted (1 without abcDraftTokens)
 * @property {number} semanticTps
 * @property {number} firstLatentSeconds - generate(): wall time to the first final latent frame
 * @property {number} narSecondsPerAudioSecond - generate(): NAR cost per second of audio
 */

/**
 * A captured block of hidden states.
 * @typedef {Object} YuE2Hidden
 * @property {'plan'|'semantic'|'synthesize'} stage
 * @property {number} layer
 * @property {number} pos0 - First token position (AR) or latent frame (NAR)
 * @property {number} rows - Positions in data
 * @property {number} hidden - Floats per row (2048)
 * @property {number} narEval - NAR: the velocity evaluation; -1 on the AR path
 * @property {Float32Array} data - rows * hidden
 */

/**
 * @typedef {Object} YuE2Result
 * @property {YuE2Plan} plan
 * @property {string} abc - Same as plan.abc
 * @property {Int32Array} semantic - Codec ids
 * @property {boolean} semanticTruncated
 * @property {Float32Array} latents - frames * 64, frame-major
 * @property {number} frames
 * @property {YuE2Timing} timing
 * @property {boolean} cancelled
 * @property {YuE2Audio} [audio] - With vae: the whole decoded song
 * @property {YuE2Hidden[]} [hidden] - With capture
 */

/**
 * Planar stereo PCM.
 * @typedef {Object} YuE2Audio
 * @property {Float32Array} samples - channels * frames, planar (channel 0 then channel 1)
 * @property {number} channels - 2
 * @property {number} frames - Samples per channel
 * @property {number} sampleRate - 48000
 */

/**
 * A streamed chunk: YuE2Audio plus where it starts.
 * @typedef {Object} YuE2AudioChunk
 * @property {Float32Array} samples
 * @property {number} channels
 * @property {number} frames
 * @property {number} sampleRate
 * @property {number} sample0 - Offset of the chunk's first sample in the song
 */

/**
 * @typedef {Object} YuE2VaeDecodeOptions
 * @property {number} [coreFrames] - Tile the decode with this many frames per tile (exact); 0 = one pass (or 50 when streaming)
 * @property {number} [haloFrames] - Context per side; at least requiredHalo(coreFrames)
 * @property {number} [firstFrames=8] - With onChunk: the first tile; tiles double from it up to coreFrames, so the
 *   first chunk arrives after one small tile instead of a full core
 * @property {function(YuE2AudioChunk)} [onChunk] - Stream: chunks in order as tiles finish (async)
 * @property {function({done: number, total: number})} [onProgress] - Tiles done (async, without onChunk)
 * @property {function(YuE2Audio|undefined, {cancelled: boolean, error: string})} [onDone] - Async; the result is undefined when streaming
 */

/**
 * @typedef {Object} YuE2LoadOptions
 * @property {string} [device] - 'cuda', 'vulkan', 'metal' or 'cpu'; defaults to the GPU
 * @property {boolean} [decoderOnly=false] - loadVae: skip the encoder
 * @property {string} [arWeights='int8'] - loadYuE2: the weights the one-token AR decode (plan + semantic sampling)
 *   reads. 'bf16' is the checkpoint's own, the choice for parity with the reference model. 'int8' / 'fp8' keep per-output-row quantised copies of every layer's AR projections and lm_head (~1.9 GB
 *   more VRAM) and roughly halve the bytes each token reads: the ABC plan goes from ~200 to ~314 tok/s on a 4090
 *   (cot 'full' time to first latent ~10.1 s -> ~8.1 s). Prefill, synthesis and the VAE stay BF16. 'int8' is the
 *   closer of the two (a sampled token differs from BF16's distribution ~0.3% of the time in the plan, ~1.5% in
 *   semantic sampling; fp8 0.4% / 2.5%) at the same speed.
 * @property {Function} [onReady] - Load on a worker thread; called with the model
 * @property {Function} [onError]
 */

// ── Streaming ────────────────────────────────────────────────────────────────

/**
 * One song of a stream's queue: YuE2Options' request fields (style, lyrics,
 * cot, abc, seed, cfgScale, abcSampling, semanticSampling, odeSteps;
 * steer is ignored, the knobs steer), plus:
 * @typedef {Object} YuE2Song
 * @property {Int32Array} [semantic] - Codec ids (0..32767) to render instead of sampling (offline renders, tests)
 * @property {string} [tag] - Your label, echoed in events and stats
 */

/**
 * Steering knobs. Each is clamped to its range (stream.knobRanges, and
 * bro.music.knobRanges before a stream exists), which
 * stops short of where yue2-research found the music breaking.
 * @typedef {Object} YuE2Knobs
 * @property {number} [energy=0] - Latent offset along the energy direction, [-2, 1]: louder, brighter; heard within the emit lead
 * @property {number} [texture=0] - NAR steer after layer 21, [-2, 2]; re-solved from knobLatencySeconds ahead
 * @property {number} [percussion=0] - NAR steer after layer 9, [-6, 6]: onsets
 * @property {number} [arrangement=0] - Semantic steer after layer 21, [-2, 1]: a real arrangement change; the song continues under it from about 2.5 s ahead
 * @property {number} [density=0] - Semantic style steer after layer 24, [-1.5, 1]: full band (+) or sparse (-); steered like arrangement
 * @property {number} [timbre=0] - Semantic style steer after layer 24, [-1.5, 1]: bright (+) or warm and dark (-); steered like arrangement
 * The style axes are yue2-research's paired-contrast directions (exp2c,
 * exp3): both are clean to -1 and lose one clip in four at -2; at +1 the
 * spectrum flattens about 6x, as arrangement's does at its cap. A tempo-feel
 * direction did not change the onset rate, so tempo is not a knob: it is
 * the plan's Q: header (setPlan, or a "N bpm" in the song's style).
 * knobRanges lists the axes this build carries.
 */

/**
 * @typedef {Object} YuE2StreamOptions
 * @property {YuE2Vae} vae - Required
 * @property {YuE2Song[]} [songs] - The initial queue (enqueue() adds more)
 * @property {function(YuE2AudioChunk)} [onAudio] - The program audio, contiguous from sample 0 (chunk.sample0), planar stereo at 48 kHz
 * @property {function(YuE2StreamEvent)} [onEvent]
 * @property {function({song: number, frame0: number, frames: number, latents: Float32Array})} [onLatents] - Each landed NAR window's latents (before the energy offset; a re-solve repeats frames)
 * @property {function({cancelled: boolean, error: string})} [onDone] - Once, after stop()
 * @property {boolean} [paced=true] - false: render as fast as possible, no playhead (ends when the queue is played out, radio off)
 * @property {boolean} [radio=true] - A want left unanswered for wantGraceSeconds is filled from the last song's request, next seed
 * @property {number} [aheadSongs=1] - Songs kept built after the playing one (1..8), one built at a time
 * @property {number} [wantGraceSeconds=10] - Radio: how long a want waits for enqueue() before the radio fills it
 * @property {number} [previewSeconds=0] - 10..600: build each song as a preview of this length (makeFull continues it); 0 = whole songs
 * @property {boolean} [autoFull=false] - Previews: each song is made full as it goes on air
 * @property {number[]} [windows=[250, 500]] - NAR window sizes in frames; the last repeats
 * @property {number} [left=25] - Inpainted left context frames
 * @property {number} [right=25] - Lookahead frames
 * @property {number} [emitLeadSeconds=0.75] - Audio handed out ahead of the playhead
 * @property {number} [decodeLeadSeconds=6] - Audio decoded ahead of the playhead
 * @property {number} [crossfadeSeconds=3] - Song to song, equal power, clamped to 2..4
 * @property {number} [skipFadeSeconds=0.5]
 * @property {number} [interruptLatencySeconds=1.75] - Interrupt stage 2's starting estimate of its own latency (then measured)
 * @property {number} [knobLatencySeconds=1.5] - Texture / percussion re-solve from this far ahead
 * @property {YuE2Knobs} [knobs] - Initial knobs
 */

/**
 * What a stream reports. `song` is the song's index in queue order from 0;
 * `sample` is a program sample.
 * @typedef {Object} YuE2StreamEvent
 * @property {'plan'|'songStart'|'semanticDone'|'window'|'interrupt'|'knobs'|'breakWarning'|'songEnd'|'firstAudio'|'error'|'idle'|'want'|'built'|'dropped'|'full'} kind
 *   - plan: a song's score is ready, text = the ABC (a queued song's can be edited: setPlan / setNextPlan)
 *   - songStart: the song is placed on the program at `sample` (a skip re-places it, and the songs after it;
 *     after a skip, seconds = the skip request to the cut-over)
 *   - semanticDone: frames = the song's length in frames
 *   - window: a NAR window landed (frame0, frames); seconds = its solve time, value = its mean latent norm
 *   - interrupt: stage `value` (1 or 2) audible from `sample`; seconds = request to audible on the playhead
 *   - knobs: knobs applied to `song` (text = which); sample = the emit frontier then, frame0 = the song frame
 *     the change starts at (0 = the whole song, rebuilt; -1 = nothing to redo)
 *   - breakWarning: a window's latent norm left the corpus range 6.5..9.5 (value = the norm); a steer may be breaking the audio
 *   - songEnd: sample = one past the song's last program sample
 *   - firstAudio: seconds = the song's request (or skip) to its first audio handed out
 *   - error: text = the message; the song is dropped (one on air ends where its audio does)
 *   - idle: nothing queued and everything played out (radio off)
 *   - want: fewer than aheadSongs wait after the playing one; value = how many more songs it wants (again
 *     whenever the shortfall grows), frames = the songs it has
 *   - built: the song is built and waits as audio (frames = its length, seconds = its request to built);
 *     a skip to it is instant
 *   - dropped: drop() removed the song
 *   - full: makeFull() (or autoFull) acted: value 1 = the preview continues into the whole song, re-solved from
 *     frame0 (the join); 0 = too late (its successor is on air, or a skip cut it) or its song had ended
 * @property {number} song
 * @property {string} tag
 * @property {number} sample
 * @property {number} frame0
 * @property {number} frames
 * @property {number} seconds
 * @property {number} value
 * @property {string} text
 */

/**
 * @typedef {Object} YuE2StreamStats
 * @property {number} playhead - Program samples played (as last reported)
 * @property {number} emitted - Program samples handed out
 * @property {YuE2Knobs} knobs - As applied
 * @property {number} firstAudioSeconds
 * @property {number} interrupt1Seconds - The last interrupt's stage 1, request to audible
 * @property {number} interrupt2Seconds
 * @property {Array<{index: number, tag: string, planned: boolean, semanticDone: boolean, semanticFrames: number, latentFrames: number, decodedFrames: number, start: number, end: number, latentNorm: number, built: boolean, ready: boolean, onAir: boolean, hostBytes: number, knobs: YuE2Knobs, preview: boolean, previewFrames: number, estimateFrames: number, ended: boolean}>} songs - Queued and playing, in order; start / end are program samples (-1 = not yet known); built = wholly decoded; ready = a skip to it is instant; knobs = what it carries (energy: its offset now); preview = still a preview of previewFrames; estimateFrames = the plan's length estimate from its score (0 = no score), for a full song's progress; ended = its semantic stage reached the song's own end
 * @property {number} ahead - Songs after the playing one
 * @property {number} readyAhead - Of those, ready for an instant skip
 * @property {number} hostBytes - Host memory the songs' audio, latents and tokens take
 */

/**
 * A running stream (yue.stream). Every control returns at once; the worker
 * applies it between steps.
 */
class YuE2Stream {
  /** @param {YuE2Song} song */
  enqueue(song) {}
  /** Program samples the player has played; paces the stream. @param {number} samples */
  setPlayhead(samples) {}
  /**
   * Merge knobs in (fields left out keep their value).
   * @param {YuE2Knobs} knobs
   * @returns {YuE2Knobs} the knobs now, clamped
   */
  setKnobs(knobs) {}
  /** Two-stage interrupt: +1 energetic, -1 calm (|amount| <= 1). @param {number} [amount=1] */
  interrupt(amount) {}
  /** To the next song (built, or as soon as it is), over skipFadeSeconds. */
  skip() {}
  /** The next song's score (from its 'plan' event, edited); it is rebuilt from the plan on. @param {string} abc */
  setNextPlan(abc) {}
  /**
   * A queued song's score (from its 'plan' event, edited): rebuilt from the
   * plan on, built or not; ignored once it is on air.
   * @param {number} song - its index
   * @param {string} abc
   */
  setPlan(song, abc) {}
  /**
   * Remove a queued song that is not on air, built or not; the songs after
   * it move up, a 'dropped' event follows, and a 'want' if the pool is short.
   * @param {number} song - its index
   */
  drop(song) {}
  /**
   * Continue a preview into the whole song (see Previews above); a 'full'
   * event follows. A song not on air yet is built whole when its turn comes.
   * @param {number} [song] - its index; omitted = the song on air
   */
  makeFull(song) {}
  /** Previews: make each song full as it goes on air (true), or not. @param {boolean} on */
  setAutoFull(on) {}
  /** End the stream; onDone follows. */
  stop() {}
  /** @readonly @type {YuE2StreamStats} */ stats;
  /** @readonly @type {YuE2Knobs} */ knobs;
  /** @readonly @type {boolean} */ running;
  /** Each knob's clamp. @readonly @type {Object<string, {min: number, max: number}>} */ knobRanges;
}

// ── Classes ──────────────────────────────────────────────────────────────────

/**
 * A loaded YuE2-3B backbone.
 */
class YuE2 {
  /** @readonly @type {boolean} */ loaded;
  /** @readonly @type {string} */ device;
  /** True while an op runs on this model. @readonly @type {boolean} */ busy;
  /** The AR decode weights it was loaded with: 'bf16' | 'int8' | 'fp8'. @readonly @type {string} */ arWeights;
  /** 64. @readonly @type {number} */ latentDim;
  /** Latent frames per second (25). @readonly @type {number} */ frameRate;

  /**
   * The whole pipeline. Cancel with handle.cancel(): the job stops at the next
   * token or ODE step, and onDone gets what exists with cancelled = true.
   * @param {YuE2Options} opts
   * @returns {{cancel: function(), wait: function()}} AsyncHandle
   */
  generate(opts) {}

  /**
   * The plan stage only; onDone's result.plan is the score.
   * @param {YuE2Options} opts
   * @returns {{cancel: function(), wait: function()}} AsyncHandle
   */
  plan(opts) {}

  /**
   * The semantic stage from a plan; onDone's result.semantic.
   * @param {YuE2Plan} plan
   * @param {YuE2Options} opts
   * @returns {{cancel: function(), wait: function()}} AsyncHandle
   */
  semantic(plan, opts) {}

  /**
   * The NAR stage from a plan and semantic ids; onDone's result.latents (and
   * result.audio with opts.vae).
   * @param {YuE2Plan} plan
   * @param {Int32Array} semantic
   * @param {YuE2Options} opts
   * @returns {{cancel: function(), wait: function()}} AsyncHandle
   */
  synthesize(plan, semantic, opts) {}

  /**
   * Streaming playback, the radio and live steering (see the top of this
   * file). Holds the model and opts.vae until it stops.
   * @param {YuE2StreamOptions} opts
   * @returns {YuE2Stream}
   */
  stream(opts) {}

  /** @param {string} text @returns {Int32Array} */
  tokenize(text) {}

  /** @param {Int32Array} ids @returns {string} */
  detokenize(ids) {}
}

/**
 * The YuE2 VAE: 48 kHz stereo <-> 64-dim latents at 25 frames per second
 * (1920 samples a frame). A decode of T frames is 1920*T - 64 samples per
 * channel.
 */
class YuE2Vae {
  /** @readonly @type {boolean} */ loaded;
  /** @readonly @type {string} */ device;
  /** @readonly @type {boolean} */ busy;
  /** False when loaded decoderOnly. @readonly @type {boolean} */ hasEncoder;
  /** 48000. @readonly @type {number} */ sampleRate;
  /** 2. @readonly @type {number} */ channels;
  /** 64. @readonly @type {number} */ latentDim;
  /** 1920. @readonly @type {number} */ downsamplingRatio;
  /** 25. @readonly @type {number} */ frameRate;

  /**
   * The smallest halo that makes a tile of coreFrames exact (12).
   * @param {number} [coreFrames]
   * @returns {number}
   */
  requiredHalo(coreFrames) {}

  /**
   * Samples per channel decoded from `frames` latents.
   * @param {number} frames
   * @returns {number}
   */
  outputLength(frames) {}

  /**
   * Latents (frame-major, frames * 64) to audio. Sync without onDone /
   * onChunk; otherwise an AsyncHandle.
   * @param {Float32Array} latents
   * @param {YuE2VaeDecodeOptions} [opts]
   * @returns {YuE2Audio|{cancel: function(), wait: function()}}
   */
  decode(latents, opts) {}

  /**
   * Audio to posterior-mean latents (for covers and edits). `audio` is planar
   * { samples, channels, sampleRate }, or mono (a Float32Array at 48 kHz, or
   * { samples, sampleRate }); other rates are resampled, and mono feeds both
   * channels. Encodes in exact tiles, so a whole song fits on the GPU. Sync
   * without opts.onDone.
   * @param {YuE2Audio|Float32Array|{samples: Float32Array, sampleRate: number}} audio
   * @param {{onDone?: function({latents: Float32Array, frames: number}|undefined, {cancelled: boolean, error: string})}} [opts]
   * @returns {{latents: Float32Array, frames: number}|{cancel: function(), wait: function()}}
   */
  encode(audio, opts) {}
}

// ── Namespace ────────────────────────────────────────────────────────────────

/** False in a build without BRO_WITH_SOUNDML (every call then throws). @type {boolean} */
bro.music.available;

/** Probe and initialise the tensor backends (idempotent). */
bro.music.init = function() {};

/**
 * Load YuE2-3B from its folder.
 * @param {string} modelDir - config.json, model.safetensors, qwen.tiktoken
 * @param {YuE2LoadOptions} [opts]
 * @returns {YuE2|{cancel: function(), wait: function()}} the model, or an AsyncHandle with opts.onReady
 */
bro.music.loadYuE2 = function(modelDir, opts) {};

/**
 * Load the YuE2 VAE from its folder.
 * @param {string} vaeDir - config.json, model.safetensors
 * @param {YuE2LoadOptions} [opts]
 * @returns {YuE2Vae|{cancel: function(), wait: function()}} the VAE, or an AsyncHandle with opts.onReady
 */
bro.music.loadVae = function(vaeDir, opts) {};

/**
 * Each stream knob's clamp, with no model or stream: the object a stream's
 * knobRanges gives (the same constants Yue2Knobs clamps with), a fresh one
 * each read. For a UI's sliders before Start.
 * @readonly @type {Object<string, {min: number, max: number}>}
 * @example
 *   bro.music.knobRanges.percussion;   // { min: -6, max: 6 }
 */
bro.music.knobRanges;
