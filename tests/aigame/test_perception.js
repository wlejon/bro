// Line of sight, computeAim, computeLeadAim.

const G = bro.ai.game;

// Clear LOS through empty world.
assert(G.hasLineOfSight(0, 0, 10, 0, []) === true, 'clear LOS empty obstacles');

// Blocked LOS through wall in middle.
const wall = [{ x: 5, z: 0, hw: 1, hd: 5 }];
assert(G.hasLineOfSight(0, 0, 10, 0, wall) === false, 'blocked LOS through wall');

// Around wall: should be clear
assert(G.hasLineOfSight(0, 10, 10, 10, wall) === true, 'LOS above wall is clear');

// Endpoints on either side of small obstacle but path skirts around — still
// blocked if direct line crosses.
const block = [{ x: 5, z: 5, hw: 1, hd: 1 }];
assert(G.hasLineOfSight(5, 0, 5, 10, block) === false, 'blocked: straight line through small obstacle');
assert(G.hasLineOfSight(0, 0, 10, 0, block) === true,  'clear: line skirts small obstacle');

// computeAim: shape + sanity
{
    const aim = G.computeAim(0, 1.6, 0, 0, 1.6, -10);
    assert(typeof aim === 'object' && 'yaw' in aim && 'pitch' in aim, 'computeAim returns yaw/pitch');
    assert(Number.isFinite(aim.yaw) && Number.isFinite(aim.pitch), 'aim is finite');
    // Target straight ahead -Z: yaw should be ~0, pitch ~0.
    assert(Math.abs(aim.yaw) < 0.05, 'forward yaw ~ 0, got ' + aim.yaw);
    assert(Math.abs(aim.pitch) < 0.05, 'forward pitch ~ 0, got ' + aim.pitch);
}

// computeAim: target above
{
    const aim = G.computeAim(0, 0, 0, 0, 10, -10);
    assert(aim.pitch > 0, 'pitch positive looking up, got ' + aim.pitch);
}

// computeLeadAim: stationary target — should be valid and roughly equal computeAim
{
    const direct = G.computeAim(0, 1.6, 0, 10, 1.6, -5);
    const lead = G.computeLeadAim(0, 1.6, 0, 10, 1.6, -5, 0, 0, 0, 40);
    assert(lead.valid === true, 'lead valid for stationary target');
    assert(Number.isFinite(lead.yaw) && Number.isFinite(lead.pitch), 'lead yaw/pitch finite');
    assert(typeof lead.timeToHit === 'number', 'timeToHit is number');
    assert(lead.timeToHit > 0, 'positive timeToHit, got ' + lead.timeToHit);
    assert(Math.abs(lead.yaw - direct.yaw) < 0.05, 'stationary lead ≈ direct aim yaw');
}

// computeLeadAim: moving target — yaw should differ from direct aim
{
    const direct = G.computeAim(0, 1.6, 0, 10, 1.6, 0);
    const lead = G.computeLeadAim(0, 1.6, 0, 10, 1.6, 0, 0, 0, 10, 40);
    assert(lead.valid === true, 'lead valid for moving target');
    // Target moving +z at 10 with projectile speed 40 — lead point differs.
    assert(Math.abs(lead.yaw - direct.yaw) > 0.01, 'lead aim differs from direct, drift=' +
        Math.abs(lead.yaw - direct.yaw));
}

// computeLeadAim: target outrunning projectile — should report invalid
{
    const lead = G.computeLeadAim(0, 0, 0, 10, 0, 0, 100, 0, 0, 1);
    // Target moving 100 u/s away with projectile speed 1 — no intercept possible.
    // Spec: valid=false in this case.
    if (lead.valid === true) {
        // BUG: lead-aim reports valid intercept for unreachable target
        assert(false, 'BUG: computeLeadAim claimed valid=true for target outrunning projectile');
    }
    assert(lead.valid === false, 'unreachable target: valid=false');
}

console.log('test_perception: OK');
