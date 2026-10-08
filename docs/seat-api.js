/**
 * @file docs/seat-api.js
 * @summary Documentation and examples for the `bro.seat` JavaScript API.
 */

/**
 * `bro.seat` provides session state inspection, lock/unlock control, virtual terminal
 * switching, idle/sleep/shutdown inhibition, and XDG autostart discovery and launching:
 * - Session state query (`getSessionState`)
 * - Screen lock and unlock control (`lock`, `unlock`)
 * - Virtual terminal switching (`switchVt`)
 * - Idle / sleep / shutdown inhibition handles (`inhibit`, `uninhibit`, `listInhibitors`)
 * - An idle timer for auto-lock / screen blanking (`setIdleTimeout`, `getIdleTimeout`,
 *   `getIdleState`, the `idle` event)
 * - XDG Autostart management (`listAutostart`, `runAutostart`)
 * - Session event listening (`on`, `off`, `addEventListener`, `removeEventListener`)
 *
 * Privileged API: Mounted automatically in Bronze when `BRO_WITH_SEAT` is enabled and
 * the app is verified as a trusted shell app declaring `"shell": true` or
 * `"privileged": ["seat", ...]` in its `bro.json`.
 */

// ============================================================================
// 1. Session State & VT Switching
// ============================================================================

if (bro.seat.available) {
    const session = bro.seat.getSessionState();
    console.log('Session state:', {
        active: session.active,
        locked: session.locked,
        vt: session.vt,
        id: session.id,
        user: session.user,
        seat: session.seat
    });

    // Listen for session lock and unlock events
    bro.seat.on('lock', () => {
        console.log('Session has been locked');
    });

    bro.seat.on('unlock', () => {
        console.log('Session has been unlocked');
    });

    // Switch to virtual terminal 1
    // bro.seat.switchVt(1);
}

// ============================================================================
// 2. Idle and Sleep Inhibition
// ============================================================================

// Take an idle inhibitor (e.g. during video playback or presentation)
const inhibitId = bro.seat.inhibit('idle', 'Playing full-screen presentation');
console.log(`Acquired inhibitor ID: ${inhibitId}`);

// List active inhibitors:
const inhibitors = bro.seat.listInhibitors();
for (const inh of inhibitors) {
    console.log(`Inhibitor #${inh.id} [${inh.type}]: ${inh.reason}`);
}

// Release inhibitor:
if (inhibitId > 0) {
    bro.seat.uninhibit(inhibitId);
}

// ============================================================================
// 2b. Idle Timer (auto-lock, screen blanking)
// ============================================================================

// The timer is off until a timeout is set. It counts from the last user input
// bro itself saw: under `bro --drm` that is every libinput event (keys,
// pointer, wheel, touch), whether it went to the shell or to a client window;
// in a window it is input to bro's own window; headless it is injected input
// (keyDown, mouseMove, ...) on advanceTime()'s virtual clock.
bro.seat.setIdleTimeout(5 * 60 * 1000);   // returns the timeout in force; 0 turns it off
bro.seat.getIdleTimeout();                 // -> 300000

// `idle` fires on each transition: { type: 'idle', idle: true, idleTime } once
// the timeout passes with no input, { idle: false } on the next input.
bro.seat.on('idle', (e) => {
    if (e.idle) {
        console.log(`Idle for ${e.idleTime} ms: locking`);
        // e.g. show the lock screen, then bro.seat.lock()
    } else {
        console.log('Activity again');
    }
});

// While any idle inhibitor is held the timer does not fire, and releasing the
// last one restarts the countdown (the screen does not lock the moment a video
// ends). Counted: inhibitors this app took with bro.seat.inhibit('idle', ...),
// logind idle inhibitors of other programs (polled every few seconds), and
// under DRM a client of bro's compositor holding zwp_idle_inhibit_v1 on a
// visible surface (a playing video).
const st = bro.seat.getIdleState();   // { idle, idleTime, timeout, inhibited }

// ============================================================================
// 3. XDG Autostart Management
// ============================================================================

// Discover autostart desktop entries
const autostartEntries = bro.seat.listAutostart();
for (const entry of autostartEntries) {
    console.log(`Autostart: ${entry.name} (${entry.id}) - exec: ${entry.exec}, enabled: ${entry.enabled}`);
}

// Launch all enabled autostart entries asynchronously:
bro.seat.runAutostart().then(results => {
    for (const res of results) {
        console.log(`Launched ${res.id}: PID ${res.pid}, success=${res.success}`);
    }
});
