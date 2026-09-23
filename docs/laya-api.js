/**
 * =============================================================================
 * bro.lm.loadLaya — Laya, a realtime decision model (the Laya half of bro.lm)
 * =============================================================================
 *
 * Laya answers typed questions about a state in one forward pass: give it a
 * STATE (text, an email, a ticket, any JSON) and QUESTIONS, each one of
 *
 *   choice  pick one of N named options           -> { choice, probabilities, confidence }
 *   score   an ordinal scale, option i = value i   -> { score (expected value), probabilities, confidence }
 *   noul    a yes/no probability                   -> { noul = p(true), confidence }
 *
 * and it returns calibrated probabilities. It never generates text: every
 * option is scored at its own [MASK] marker and softmaxed over that
 * question's options, so the answer space is defined per request and there
 * is nothing to parse. The checkpoint is the English `laya` (ModernBERT-large
 * encoder + a 2-layer decision head, 421M params, 512-token context of which
 * `head_max_len` = 192 hold the question and its options). Backed by brolm on
 * brotensor, FP16 on CUDA.
 *
 * WHAT IT IS GOOD AT (the checkpoint's own in-task evaluation, after
 * calibration): intent / routing (0.99 accuracy), moderation and safety
 * (0.97), topic classification (0.94), emotion and tone (0.91), NLI and fact
 * checking (0.88), instruction following (0.88, and it holds up zero-shot:
 * 0.86), email triage (0.73).
 *
 * WHAT IT IS BAD AT — design around these, don't discover them in production:
 *  - `score` questions are the weakest primitive (fine-grained sentiment /
 *    rating 0.44, response-quality scoring 0.58). Prefer a `choice` over a few
 *    named bands, or a `noul`, when you can phrase it that way.
 *  - More than ~20 options. The options share the 192-token head budget, so
 *    77 labels get 3-4 tokens each and blur together (Banking77: 0.43). Raise
 *    `headMaxLen` / `maxLen` per call, or split into a coarse -> fine pair of
 *    questions. Options that cannot fit at all raise ("options do not fit")
 *    rather than answer over a truncated set.
 *  - Most zero-shot use. Held-out task families score 0.65 overall (emotion
 *    0.58, sentiment 0.36); the published typed-decisions benchmark is near
 *    chance on this base checkpoint. It is a fast base to fine-tune, not a
 *    general zero-shot decision engine.
 *  - Non-English text: this is the English checkpoint, and it stays confident
 *    while wrong on other scripts. Use a multilingual checkpoint for those.
 *
 * ESCALATION: gate on `confidence`, not on `rl_agent.act_probability`. The
 * act/escalate head is saturated — act_probability is ~1 for essentially
 * every input (the PyTorch reference behaves the same), so it carries no
 * signal. `confidence` is 1 - H(p)/log(K) of the temperature-calibrated
 * distribution: route low-confidence answers to a human or a bigger model.
 * The shipped temperatures are the checkpoint's; for probabilities you rely
 * on, refit one temperature per (question type, option count) on your own
 * data — every answer carries its raw pre-temperature `logits` for that.
 *
 * REALTIME SERVING. A LayaModel is a request scheduler over one model replica
 * per GPU (`devices`). Every predict/predictAsync call — from this app, its
 * Workers, anything — joins one queue; whichever replica is idle packs what
 * is queued into its next forward, so concurrent calls batch together and
 * load spreads over the GPUs. A forward is capped at a token budget sized so
 * it takes about `targetForwardMs` (12 ms by default, measured at load), which
 * bounds how long a new request can wait behind a batch already running;
 * admission is by `priority`, then deadline, then arrival, per question, so a
 * big request splits across forwards and idle GPUs. Missed deadlines are
 * counted in stats(), never dropped. All CUDA-graph shapes up to the budget
 * are captured at load (~1-2 s), so no live request pays for one.
 *
 * Measured on an RTX 4090 (submit -> result, tokenize included): one 5-
 * question request on a ~90-token email ~5 ms, one question ~2 ms. Under
 * Poisson arrivals of 5-question email-sized requests, one GPU holds p99
 * under 30 ms up to ~175 requests/s (875 decisions/s), two GPUs ~350/s;
 * ~200-token conversation states about half that. The larger shapes are
 * GEMM-bound: a forward costs ~7.7 us per packed token plus ~1.5 ms.
 *
 * predictAsync promises settle on the frame they complete (the LM tick runs
 * in the engine's frame pump and drains microtasks after it), so JS sees a
 * result up to one frame after the device finished it — `timing.totalMs` is
 * the engine's own submit -> result time.
 *
 * @example
 *   // --- Load (blocking) and ask -------------------------------------------
 *   const laya = bro.lm.loadLaya('../laya', { devices: 'all' });
 *   const res = laya.predict(
 *     { from: 'user@acme.com', subject: 'Duplicate charge on invoice #4411',
 *       body: 'We were billed twice for March. Refund the duplicate today or we cancel.' },
 *     {
 *       department: { type: 'choice', instructions: 'Which department should handle this request?',
 *                     criteria: { billing: 'invoices, payments, refunds', technical: 'bugs, outages',
 *                                 sales: 'pricing, new contracts', other: 'everything else' } },
 *       urgency:    { type: 'score', instructions: 'How urgent is this request?',
 *                     criteria: ['not urgent', 'soon', 'critical deadline or blocking issue'] },
 *       churn_risk: { type: 'noul', instructions: 'Does the user threaten to cancel or leave?' },
 *     });
 *   res.answers.department.choice;      // 'billing'
 *   res.answers.department.confidence;  // ~0.9: act on it
 *   res.answers.churn_risk.noul;        // p(true)
 *
 * @example
 *   // --- Many requests in flight: they share forwards ----------------------
 *   const laya = await bro.lm.loadLayaAsync('../laya', { devices: 'all' });
 *   const tickets = inbox.map((t) => laya.predictAsync(t, QUESTIONS, { deadlineMs: 30 }));
 *   for (const r of await Promise.all(tickets)) {
 *     const d = r.answers.department;
 *     if (d.confidence < 0.6) escalate(r);       // confidence, not act_probability
 *     console.log(d.choice, r.timing.totalMs.toFixed(1) + ' ms',
 *                 'batched with', r.timing.batchRequests - 1, 'others');
 *   }
 *
 * @example
 *   // --- Priority: an interactive request overtakes a background backlog ---
 *   laya.predictAsync(userMessage, QUESTIONS, { priority: 10, deadlineMs: 20 });
 *   backlog.forEach((doc) => laya.predictAsync(doc, QUESTIONS, { priority: 0, deadlineMs: 2000 }));
 *
 * @example
 *   // --- Watching the engine -----------------------------------------------
 *   const s = laya.stats();
 *   console.log(s.latencyP99, 'ms p99 at', s.throughputRps.toFixed(0), 'req/s;',
 *               s.meanBatchRequests.toFixed(1), 'requests per forward,',
 *               (s.meanOccupancy * 100).toFixed(0) + '% of the token budget');
 */

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * One question. `criteria` depends on `type`:
 *  - choice: `{ key: description, ... }` (description may be '' or null),
 *    an array of keys, or an array of `[key, description]` pairs.
 *  - score: an array of level descriptions, lowest first (an object
 *    contributes its keys). The answer's `score` is the expected level index.
 *  - noul: optional `{ false: description, true: description }` or
 *    `[falseDescription, trueDescription]`.
 * A missing `type` is inferred (choice if criteria names options, else noul).
 * Non-string `instructions` are serialised like Python's json.dumps.
 * @typedef {Object} LayaQuestion
 * @property {string} [type] - 'choice' | 'score' | 'noul'
 * @property {(string|Object)} instructions
 * @property {(Object|Array)} [criteria]
 */

/**
 * Options of loadLaya / loadLayaAsync (also accepted on the first argument
 * alongside `path`).
 * @typedef {Object} LoadLayaOptions
 * @property {string} [path] - Checkpoint directory (rl_agent_config.json, encoder/, tokenizer/, model.safetensors); `modelPath` is an alias
 * @property {(string|number|Array<number>)} [devices] - 'all' = one replica per CUDA device; an index; or a list. Default: the default device only
 * @property {number} [targetForwardMs=12] - Forward-time target the per-forward token budget is derived from (<= 0: use maxBatchTokens)
 * @property {number} [maxBatchTokens=2048] - Hard cap on packed tokens per forward, and the range pre-warm covers
 * @property {number} [deadlineMs=30] - Deadline of a request that names none
 * @property {boolean} [prewarm=true] - Capture every CUDA-graph bucket up to maxBatchTokens at load
 * @property {number} [maxLen] - Load-time override of the checkpoint's max_len (512)
 * @property {number} [headMaxLen] - Load-time override of head_max_len (192)
 */

/**
 * Per-call options of predict / predictAsync.
 * @typedef {Object} LayaPredictOptions
 * @property {number} [priority=0] - Higher runs first
 * @property {number} [deadlineMs] - Relative to the call; default the model's deadlineMs. Late requests still run, and count as missed
 * @property {number} [maxLen] - Sequence limit for this call (tokens)
 * @property {number} [headMaxLen] - Question + options budget for this call (tokens); raise it for many options
 * @property {boolean} [truncateLeft=false] - Keep the newest state tokens when the state is too long (conversations)
 */

/**
 * One answer. `choice` / `score` answers carry `probabilities` keyed by
 * option (score: '0', '1', ...); `noul` answers carry `noul` = p(true).
 * @typedef {Object} LayaAnswer
 * @property {string} type
 * @property {string} [choice] - The argmax option key (choice)
 * @property {number} [score] - Expected level index (score)
 * @property {Object<string,string>} [legend] - Level index -> description (score)
 * @property {number} [noul] - p(true) (noul)
 * @property {Object<string,number>} [probabilities] - Calibrated distribution (choice, score)
 * @property {number} confidence - 1 - H(p)/log(K) of the calibrated distribution; gate escalation on this (noul's is a bro extension, the reference omits it)
 * @property {Float32Array} logits - Raw pre-temperature scorer logits, option order
 * @property {number} temperature - The temperature applied (per type / option-count bucket)
 * @property {{act_probability: number, act_logits: Float32Array}} rl_agent - The act/escalate head. Saturated (~1 always): do not gate on it
 */

/**
 * How a request was served.
 * @typedef {Object} LayaTiming
 * @property {number} tokenizeMs - On the calling thread, before queueing
 * @property {number} queueMs - Queued until the first forward carrying it started
 * @property {number} forwardMs - First forward start -> last forward end
 * @property {number} totalMs - Submit -> result ready in the engine (JS sees it at the next frame pump)
 * @property {number} forwards - Forwards its questions were spread over
 * @property {number} batchItems - Questions in the (last) forward that carried it, from every request
 * @property {number} batchRequests - Requests sharing that forward
 * @property {number} batchTokens - Packed tokens in that forward
 * @property {number} device - GPU that ran it
 * @property {boolean} deadlineMissed
 */

/**
 * @typedef {Object} LayaResult
 * @property {string} model - 'rl-agent'
 * @property {Object<string, LayaAnswer>} answers - By question id, in question order
 * @property {{input_tokens: number, output_tokens: number}} usage
 * @property {LayaTiming} timing
 */

/**
 * Scheduler counters since load or the last resetStats(). Latency
 * percentiles cover the most recent 4096 completions.
 * @typedef {Object} LayaStats
 * @property {number} submitted
 * @property {number} completed
 * @property {number} failed
 * @property {number} deadlineMissed
 * @property {number} queuedRequests - Waiting for a forward now
 * @property {number} queuedItems - Their questions
 * @property {number} inFlightRequests - Submitted and not finished (queued included)
 * @property {number} forwards
 * @property {number} itemsRun
 * @property {number} tokensRun
 * @property {number} meanBatchItems
 * @property {number} meanBatchTokens
 * @property {number} meanBatchRequests
 * @property {number} meanOccupancy - Mean forward tokens / token budget
 * @property {number} tokenBudget - Tokens per forward the scheduler packs up to
 * @property {number} targetForwardMs
 * @property {number} estFixedMs - Live cost model: fixed ms per forward
 * @property {number} estMsPer1kTokens - Live cost model: ms per 1000 packed tokens
 * @property {number} latencyP50 - Submit -> result, ms
 * @property {number} latencyP95
 * @property {number} latencyP99
 * @property {number} latencyMax
 * @property {number} queueMeanMs
 * @property {number} windowS - Seconds since the stats window opened
 * @property {number} throughputRps - Completed requests per second over the window
 * @property {Array<{device: number, name: string, forwards: number, busyMs: number, busyFraction: number, graphs: number}>} devices
 * @property {Array<{seq: number, device: number, requests: number, items: number, tokens: number, budget: number, startMs: number, ms: number}>} recentBatches - The last 128 forwards, oldest first
 */

// ── Classes ──────────────────────────────────────────────────────────────────

/**
 * A loaded Laya model: the request scheduler and its replicas.
 * `new bro.lm.LayaModel(path, opts)` is loadLaya.
 */
class LayaModel {

  /**
   * Answer `questions` about `state`, blocking until done. Shares forwards
   * with every other request in flight.
   *
   * @param {(string|Object)} state - Text, or an object/array (serialised like Python's json.dumps, as the model was trained)
   * @param {Object<string, LayaQuestion>} questions - By id; answered in key order
   * @param {LayaPredictOptions} [opts]
   * @returns {LayaResult}
   * @throws {Error} "options do not fit" when a question's options exceed headMaxLen
   */
  predict(state, questions, opts) {}

  /**
   * The same, without blocking: resolves on the JS thread once answered.
   * Rejects (never throws) on bad arguments, options that do not fit, or a
   * disposed model.
   *
   * @param {(string|Object)} state
   * @param {Object<string, LayaQuestion>} questions
   * @param {LayaPredictOptions} [opts]
   * @returns {Promise<LayaResult>}
   */
  predictAsync(state, questions, opts) {}

  /**
   * Checkpoint and scheduler configuration.
   * @returns {{max_len: number, head_max_len: number, temperature: Float32Array,
   *            temperature_by_options: Object<string, number>, devices: Array<number>,
   *            tokenBudget: number, maxBatchTokens: number, targetForwardMs: number, deadlineMs: number}}
   */
  config() {}

  /**
   * Scheduler counters, latency percentiles, per-device load and the recent forwards.
   * @returns {LayaStats}
   */
  stats() {}

  /** Open a new stats window (counters, latencies, recent forwards). */
  resetStats() {}

  /**
   * Stop the device threads and free every replica's GPU memory now, rather
   * than at garbage collection. Queued requests reject; later calls throw.
   */
  dispose() {}

}

// ── bro.lm ───────────────────────────────────────────────────────────────────

/**
 * Load Laya onto one replica per requested device and pre-warm it. Blocks
 * until ready (a few seconds: weights, then graph capture). Paths resolve
 * against the app like fs.* paths.
 *
 * @param {(string|LoadLayaOptions)} pathOrOpts - Checkpoint directory, or options with `path`
 * @param {LoadLayaOptions} [opts]
 * @returns {LayaModel}
 * @throws {TypeError} without a path; {Error} when the directory holds no checkpoint or a device is missing
 */
bro.lm.loadLaya = function(pathOrOpts, opts) {};

/**
 * loadLaya without blocking the page: the replicas load and pre-warm on
 * their own threads, the promise resolves on the frame they are ready.
 *
 * @param {(string|LoadLayaOptions)} pathOrOpts
 * @param {LoadLayaOptions} [opts]
 * @returns {Promise<LayaModel>}
 */
bro.lm.loadLayaAsync = function(pathOrOpts, opts) {};
