// config.js — sim + presentation constants (hz, cadences, dims).
// Central knob file so tuning a cadence doesn't require hunting through the
// sim loop or render module.
var Config = {};
(function () {
    "use strict";

    // Arena canvas size in pixels. Render maps Arena.BOUNDS → this rect.
    Config.ARENA_W = 700;
    Config.ARENA_H = 700;

    // Fixed-step sim.
    Config.SIM_STEP = 1 / 60;         // 60 Hz sim
    Config.MAX_STEPS_PER_FRAME = 8;   // catch-up cap
    Config.MAX_SIM_ACCUM = 0.25;      // clamp accumulated lag

    // HUD panel update cadences (seconds). DOM mutations are expensive;
    // each panel is throttled independently from the sim tick.
    Config.ROSTER_HZ = 0.2;
    Config.OBS_HZ    = 0.1;
    Config.REWARD_HZ = 0.25;
    Config.STATUS_HZ = 0.25;

    // Snapshot ring buffer (for rewind button).
    Config.SNAPSHOT_INTERVAL = 1.0;
    Config.SNAPSHOT_KEEP = 5;
    Config.REWIND_SECONDS = 1.5;

    // Replay playback rate (sim tick hz = 30).
    Config.REPLAY_FRAME_DT = 0.033;

    // DAMAGE LOG cap (lines).
    Config.LOG_LINES = 60;

    // Reward chart history length (samples).
    Config.REWARD_HISTORY = 200;

    // Recording output directory (relative to working dir).
    Config.REPLAY_DIR = "apps/ai-arena/replays/";
})();
