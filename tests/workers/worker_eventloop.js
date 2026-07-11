// Companion worker for test_event_loop.js. Exercises the event loop's three
// wait regimes: idle-blocked (answers pings), timer-driven (setInterval /
// setTimeout fire while no messages arrive), and pollable-driven (fetch
// completes while no messages arrive).

let intervalTicks = 0;

onmessage = (e) => {
    const m = e.data;
    if (m.cmd === 'ping') {
        postMessage({ pong: m.n });
    } else if (m.cmd === 'startInterval') {
        // 20ms interval; each tick must fire from the deadline wait — no
        // messages arrive between startInterval and stopInterval.
        setInterval(() => {
            intervalTicks++;
            postMessage({ tick: intervalTicks });
        }, 20);
    } else if (m.cmd === 'oneshot') {
        setTimeout(() => { postMessage({ fired: m.delay }); }, m.delay);
    } else if (m.cmd === 'fetch') {
        // Resolves via the fetch pollable — the loop must keep pumping
        // __brokit_fetch_tick until the response lands.
        fetch(m.path)
            .then((r) => r.text())
            .then((text) => { postMessage({ fetched: text.trim() }); })
            .catch((err) => { postMessage({ fetchError: String(err) }); });
    } else if (m.cmd === 'close') {
        postMessage({ closing: true });
        close();
    }
};
