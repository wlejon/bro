// bro.seat's idle timer: setIdleTimeout(ms) fires `idle` events from the
// engine's own record of user input (under DRM bro reads libinput itself, so
// nobody else can tell it the user went quiet). Headless, the record is fed by
// injected input and the clock is advanceTime()'s, so this is deterministic.
assert(typeof bro.seat === 'object', 'bro.seat namespace exists');
if (!bro.seat.available || typeof bro.seat.setIdleTimeout !== 'function') {
    // Compiled out (BRO_WITH_SEAT off, Windows): the stub's methods throw.
    skipTest('bro.seat has no idle timer in this build');
} else {
    const log = [];
    bro.seat.on('idle', (e) => log.push(e.idle));

    assert(bro.seat.getIdleTimeout() === 0, 'the timer is off until asked for');
    advanceTime(5000);
    assert(log.length === 0, 'no idle event with the timer off');

    assert(bro.seat.setIdleTimeout(1000) === 1000, 'setIdleTimeout returns the timeout');
    advanceTime(600);
    assert(log.length === 0 && !bro.seat.getIdleState().idle, 'not idle before the timeout');
    advanceTime(600);
    assert(log.join() === 'true', 'idle after a quiet timeout, got ' + log.join());
    const st = bro.seat.getIdleState();
    assert(st.idle === true && st.timeout === 1000 && st.idleTime >= 1000,
           'getIdleState reports idleness: ' + JSON.stringify(st));

    // Input ends it: a key, as the DRM input path stamps every libinput event.
    keyDown(97 /* a */);
    keyUp(97);
    advanceTime(50);
    assert(log.join() === 'true,false', 'input ends idleness, got ' + log.join());

    // Steady input keeps it away.
    for (let i = 0; i < 5; i++) {
        advanceTime(500);
        mouseMove(10 + i, 10);
    }
    assert(log.join() === 'true,false', 'steady input keeps idleness away, got ' + log.join());
    advanceTime(1100);
    assert(log.join() === 'true,false,true', 'quiet again goes idle again, got ' + log.join());
    mouseMove(50, 50);
    advanceTime(20);
    assert(log.join() === 'true,false,true,false', 'pointer motion ends it too, got ' + log.join());

    // An idle inhibitor held through bro.seat.inhibit keeps the session from
    // idling (where logind grants one).
    const inh = bro.seat.inhibit('idle', 'test_seat_idle');
    if (inh > 0) {
        advanceTime(3000);
        assert(log.length === 4, 'an idle inhibitor holds idleness off, got ' + log.join());
        assert(bro.seat.getIdleState().inhibited === true, 'getIdleState reports the inhibitor');
        bro.seat.uninhibit(inh);
        advanceTime(500);
        assert(log.length === 4, 'releasing it restarts the countdown, got ' + log.join());
        advanceTime(700);
        assert(log.join() === 'true,false,true,false,true', 'then idles, got ' + log.join());
        mouseMove(60, 60);
        advanceTime(20);
    }

    bro.seat.setIdleTimeout(0);
    assert(bro.seat.getIdleTimeout() === 0, 'setIdleTimeout(0) turns it off');
    console.log('test_seat_idle.js PASSED');
}
