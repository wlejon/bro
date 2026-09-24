// The Web Animations surface that used to be missing: steps() / step-*
// / linear() easing (they fell back to `ease`, and getTiming().easing
// reported the fallback), an invalid easing throws, updatePlaybackRate(),
// commitStyles(), effect.updateTiming(), and automatic removal of replaced
// fill-forwards animations with persist() / replaceState / onremove.

const root = document.getElementById('root');
root.innerHTML = '<div id="a" style="width:10px;height:10px;opacity:1"></div>';
const el = document.getElementById('a');
flush();

function near(v, want, eps, msg) {
    assert(Math.abs(v - want) <= eps, msg + ' (expected ~' + want + ', got ' + v + ')');
}
const op = () => parseFloat(getComputedStyle(el).opacity);

// --- steps()
{
    const s = el.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 1000, easing: 'steps(4, end)' });
    assert(s.effect.getTiming().easing === 'steps(4)', 'getTiming().easing: ' + s.effect.getTiming().easing);
    s.currentTime = 100;
    assert(s.effect.getComputedTiming().progress === 0, 'steps(4) at 10% is 0');
    near(op(), 0, 1e-6, 'and so is the value');
    s.currentTime = 300;
    assert(s.effect.getComputedTiming().progress === 0.25, 'steps(4) at 30% is 0.25');
    near(op(), 0.25, 1e-6, 'value at 30%');
    s.cancel();

    const st = el.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 1000, easing: 'step-start' });
    st.currentTime = 10;
    assert(st.effect.getComputedTiming().progress === 1, 'step-start jumps at once');
    assert(st.effect.getTiming().easing === 'steps(1, start)', 'step-start reads back: ' +
           st.effect.getTiming().easing);
    st.cancel();

    // Per-keyframe steps().
    const k = el.animate([{ opacity: 0, easing: 'steps(2)' }, { opacity: 1 }], 1000);
    k.currentTime = 400;
    near(op(), 0, 1e-6, 'keyframe steps(2) holds the first half');
    k.currentTime = 600;
    near(op(), 0.5, 1e-6, 'and jumps at the half');
    assert(k.effect.getKeyframes()[0].easing === 'steps(2)', 'getKeyframes easing: ' +
           k.effect.getKeyframes()[0].easing);
    k.cancel();

    // linear() with a stop.
    const l = el.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 1000, easing: 'linear(0, 0.8 50%, 1)' });
    l.currentTime = 250;
    near(l.effect.getComputedTiming().progress, 0.4, 1e-6, 'linear() interpolates its stops');
    l.cancel();

    let threw = null;
    try { el.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 10, easing: 'wobbly' }); }
    catch (e) { threw = e instanceof TypeError; }
    assert(threw === true, 'an invalid easing throws a TypeError');
}

// --- CSS transitions take steps() too.
{
    const t = document.createElement('div');
    t.style.cssText = 'width:10px;height:10px;opacity:1;transition:opacity 1000ms steps(2, end)';
    root.appendChild(t);
    flush();
    t.style.opacity = '0';
    flush();
    advanceTime(400);
    near(parseFloat(getComputedStyle(t).opacity), 1, 1e-6, 'a steps(2) transition holds until half way');
    advanceTime(200);
    near(parseFloat(getComputedStyle(t).opacity), 0.5, 1e-6, 'then jumps');
    t.remove();
}

// --- updatePlaybackRate()
{
    const r = el.animate([{ opacity: 0 }, { opacity: 1 }], 10000);
    advanceTime(1000);
    const before = r.currentTime;
    r.updatePlaybackRate(2);
    assert(r.playbackRate === 2, 'the rate applies');
    near(r.currentTime, before, 1e-6, 'without a jump');
    advanceTime(500);
    near(r.currentTime - before, 1000, 5, 'and runs at twice the speed');
    r.cancel();
}

// --- commitStyles()
{
    const c = el.animate([{ opacity: 0 }, { opacity: 1 }], 1000);
    c.currentTime = 250;
    c.commitStyles();
    c.cancel();
    near(parseFloat(el.style.opacity), 0.25, 0.01, 'commitStyles wrote the value inline: ' + el.style.opacity);
    near(op(), 0.25, 0.01, 'and it outlasts the cancelled animation');
    el.style.opacity = '1';
}

// --- effect.updateTiming()
{
    const u = el.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 100, fill: 'forwards' });
    advanceTime(200);
    assert(u.playState === 'finished', 'finished first');
    u.effect.updateTiming({ duration: 1000, easing: 'steps(2)' });
    const t = u.effect.getTiming();
    assert(t.duration === 1000 && t.easing === 'steps(2)', 'getTiming reads the update: ' + JSON.stringify(t));
    assert(u.playState === 'running', 'a longer duration resumes a finished animation: ' + u.playState);
    let threw = false;
    try { u.effect.updateTiming({ duration: -1 }); } catch (e) { threw = e instanceof TypeError; }
    assert(threw, 'a negative duration throws');
    u.cancel();
}

// --- replaced animations are removed; persist() keeps one.
{
    const removed = [];
    const first = el.animate([{ opacity: 0.2 }, { opacity: 0.3 }], { duration: 100, fill: 'forwards' });
    first.onremove = (e) => removed.push(e.type);
    const kept = el.animate([{ opacity: 0.4 }, { opacity: 0.5 }], { duration: 100, fill: 'forwards' });
    kept.persist();
    const top = el.animate([{ opacity: 0.6 }, { opacity: 0.7 }], { duration: 100, fill: 'forwards' });
    advanceTime(200);
    advanceTime(16);
    assert(first.replaceState === 'removed', 'the covered animation is removed: ' + first.replaceState);
    assert(removed.join() === 'remove', 'onremove fired: ' + JSON.stringify(removed));
    assert(kept.replaceState === 'persisted', 'the persisted one stays: ' + kept.replaceState);
    assert(top.replaceState === 'active', 'the top one is active');
    const listed = el.getAnimations();
    assert(listed.length === 2 && listed[0] === kept && listed[1] === top,
           'getAnimations() drops the removed one: ' + listed.length);
    near(op(), 0.7, 0.01, 'the top animation still fills');
    kept.cancel();
    top.cancel();
}
