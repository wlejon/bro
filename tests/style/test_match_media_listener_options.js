// MediaQueryList.addEventListener option bag — {capture, once, signal} and the
// legacy boolean-capture third argument.
//
// Regression: the third argument was parsed off and dropped on the floor. A
// listener registered with {once: true} fired on every flip forever, and one
// registered with an AbortSignal kept firing after the controller aborted —
// both silently, since neither has any failure signal other than being called
// when it shouldn't be. `capture` was likewise ignored, so a listener added
// with capture:true and removed with the matching capture:true argument was
// removed by accident (identity ignored the flag) rather than by agreement.
//
// Flips are driven by appearance.colorScheme, the same lever the sibling
// matchMedia tests use; each set()+flush() is exactly one change event.

const QUERY = '(prefers-color-scheme: dark)';

bro.settings.set('appearance.colorScheme', 'light');
flush();

function flip(to) {
    bro.settings.set('appearance.colorScheme', to);
    flush();
}

// --- once: fires exactly once, then is gone ---------------------------------
{
    const mql = matchMedia(QUERY);
    let n = 0;
    mql.addEventListener('change', () => { n++; }, { once: true });

    flip('dark');
    assert(n === 1, 'once listener fired on the first flip, got ' + n);
    flip('light');
    assert(n === 1, 'once listener did NOT fire on the second flip, got ' + n);
    flip('dark');
    assert(n === 1, 'once listener stayed gone on a third flip, got ' + n);
    flip('light');
}

// --- once alongside a normal listener: only the once one drops --------------
{
    const mql = matchMedia(QUERY);
    let once = 0, always = 0;
    mql.addEventListener('change', () => { once++; }, { once: true });
    mql.addEventListener('change', () => { always++; });

    flip('dark');
    flip('light');
    assert(once === 1, 'once listener fired once across two flips, got ' + once);
    assert(always === 2, 'plain listener fired on both flips, got ' + always);
}

// --- a once listener that re-adds itself stays registered -------------------
// Spec removes the listener BEFORE invoking it, so re-adding inside the
// handler registers afresh rather than being undone by its own removal.
{
    const mql = matchMedia(QUERY);
    let n = 0;
    const handler = () => {
        n++;
        if (n < 3) mql.addEventListener('change', handler, { once: true });
    };
    mql.addEventListener('change', handler, { once: true });

    flip('dark');
    flip('light');
    flip('dark');
    flip('light');
    assert(n === 3, 're-adding once listener fired 3 times then stopped, got ' + n);
    flip('light');
}

// --- signal: aborting unregisters -------------------------------------------
{
    const mql = matchMedia(QUERY);
    const ctl = new AbortController();
    let n = 0;
    mql.addEventListener('change', () => { n++; }, { signal: ctl.signal });

    flip('dark');
    assert(n === 1, 'signal listener fired before abort, got ' + n);

    ctl.abort();
    flip('light');
    assert(n === 1, 'signal listener silent after abort, got ' + n);
    flip('dark');
    assert(n === 1, 'signal listener still silent on a later flip, got ' + n);
    flip('light');
}

// --- an already-aborted signal never registers at all -----------------------
{
    const mql = matchMedia(QUERY);
    const ctl = new AbortController();
    ctl.abort();
    let n = 0;
    mql.addEventListener('change', () => { n++; }, { signal: ctl.signal });

    flip('dark');
    assert(n === 0, 'pre-aborted signal never registered the listener, got ' + n);
    flip('light');
}

// --- signal aborts only its own listener ------------------------------------
{
    const mql = matchMedia(QUERY);
    const ctl = new AbortController();
    let aborted = 0, kept = 0;
    mql.addEventListener('change', () => { aborted++; }, { signal: ctl.signal });
    mql.addEventListener('change', () => { kept++; });

    flip('dark');
    ctl.abort();
    flip('light');
    assert(aborted === 1, 'aborted listener fired only before abort, got ' + aborted);
    assert(kept === 2, 'unrelated listener kept firing, got ' + kept);
}

// --- capture is part of listener identity -----------------------------------
{
    const mql = matchMedia(QUERY);
    let n = 0;
    const fn = () => { n++; };

    // Same callback at both capture values = two distinct registrations.
    mql.addEventListener('change', fn, { capture: true });
    mql.addEventListener('change', fn, { capture: false });
    flip('dark');
    assert(n === 2, 'capture:true and capture:false register separately, got ' + n);

    // Removing without the flag removes only the non-capture one.
    n = 0;
    mql.removeEventListener('change', fn);
    flip('light');
    assert(n === 1, 'plain remove left the capture listener registered, got ' + n);

    // Removing with the matching flag removes the last one.
    n = 0;
    mql.removeEventListener('change', fn, { capture: true });
    flip('dark');
    assert(n === 0, 'matching-capture remove unregistered it, got ' + n);
    flip('light');
}

// --- legacy boolean third argument means capture ----------------------------
{
    const mql = matchMedia(QUERY);
    let n = 0;
    const fn = () => { n++; };
    mql.addEventListener('change', fn, true);

    flip('dark');
    assert(n === 1, 'boolean-capture listener fired, got ' + n);

    // A plain remove must NOT match it — it was registered as capture.
    n = 0;
    mql.removeEventListener('change', fn);
    flip('light');
    assert(n === 1, 'plain remove did not match the boolean-capture add, got ' + n);

    n = 0;
    mql.removeEventListener('change', fn, true);
    flip('dark');
    assert(n === 0, 'boolean-capture remove matched, got ' + n);
    flip('light');
}

// --- a repeat add does not retroactively apply new options ------------------
// Spec: a duplicate (callback, capture) add is a no-op, so the second call's
// once:true must not convert the already-registered plain listener.
{
    const mql = matchMedia(QUERY);
    let n = 0;
    const fn = () => { n++; };
    mql.addEventListener('change', fn);
    mql.addEventListener('change', fn, { once: true });

    flip('dark');
    flip('light');
    assert(n === 2, 'duplicate add stayed a plain listener, got ' + n);
}

bro.settings.reset('appearance');

console.log('PASS matchMedia listener options');
