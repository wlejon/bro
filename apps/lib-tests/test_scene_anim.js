// Tests for scene-graph sprite animation, particles, tilemap.
// Run: bro-headless apps/lib-tests apps/lib-tests/test_scene_anim.js

'use strict';

let tests = 0, failed = 0;
function t(name, fn) {
    tests++;
    try { fn(); console.log('  ok   ' + name); }
    catch (e) {
        failed++;
        console.log('  FAIL ' + name + ': ' + (e && e.message ? e.message : e));
        if (e && e.stack) console.log(e.stack);
    }
}
function eq(a, b, msg) { if (a !== b) throw new Error((msg||'eq') + ': ' + a + ' !== ' + b); }
function truthy(v, msg) { if (!v) throw new Error(msg||'truthy'); }

console.log('=== Scene gaps tests ===');

// Set up a canvas + scene graph.
var canvas = document.createElement('canvas');
canvas.width = 320;
canvas.height = 240;
document.body.appendChild(canvas);
var scene = canvas.getContext('scene');
truthy(scene, 'scene context exists');

t('sprite advances frame index under advanceTime', function() {
    var s = scene.createSprite({
        sheet: { frameWidth: 16, frameHeight: 16, columns: 4, rows: 1 },
        animations: { walk: { frames: [0,1,2,3], fps: 10, loop: true } },
        play: 'walk'
    });
    eq(s.frameIndex, 0, 'starts at 0');
    truthy(s.isPlaying, 'should be playing');
    eq(s.currentAnimation, 'walk', 'walk active');

    // 0.25s should advance 2 frames at 10fps.
    advanceTime(250);
    eq(s.frameIndex, 2, 'frame index after 250ms');

    // Loops back: 0.5s more -> 7 total -> step 7 % 4 = 3
    advanceTime(500);
    truthy(s.frameIndex >= 0 && s.frameIndex < 4, 'frame still in range');

    s.destroy();
});

t('sprite non-looping animation fires onAnimationEnd', function() {
    var ended = null;
    var s = scene.createSprite({
        sheet: { frameWidth: 8, frameHeight: 8, columns: 3, rows: 1 },
        animations: { hit: { frames: [0,1,2], fps: 10, loop: false } },
        play: 'hit'
    });
    s.onAnimationEnd = function(name) { ended = name; };

    // 0.5s — needs ~0.3s for 3 frames at 10fps. Animation should end.
    advanceTime(500);
    eq(ended, 'hit', 'callback fired with name');
    truthy(!s.isPlaying, 'no longer playing');
    s.destroy();
});

t('sprite frameIndex direct seek', function() {
    var s = scene.createSprite({
        sheet: { frameWidth: 8, frameHeight: 8, columns: 5, rows: 1 }
    });
    s.frameIndex = 3;
    eq(s.frameIndex, 3);
    s.destroy();
});

t('particle emitter populates and decays under advanceTime', function() {
    var p = scene.createParticles({
        rate: 0,
        burst: 30,
        maxParticles: 64,
        lifetime: { min: 0.2, max: 0.2 },
        velocity: { angle: 0, angleSpread: 360, speed: 50, speedSpread: 0 },
        size: { start: 4, end: 0 },
        color: { start: '#ffffff', end: '#ffffff00' }
    });
    eq(p.liveCount, 30, 'burst populated 30');
    advanceTime(300);  // exceeds 0.2s lifetime
    eq(p.liveCount, 0, 'all expired');
    p.destroy();
});

t('particle hard cap respects maxParticles', function() {
    var p = scene.createParticles({
        rate: 0, burst: 0, maxParticles: 5,
        lifetime: { min: 1, max: 1 },
        velocity: { angle: 0, angleSpread: 0, speed: 0, speedSpread: 0 },
        size: { start: 1, end: 1 },
        color: { start: '#fff', end: '#fff' }
    });
    p.burst(20);
    eq(p.liveCount, 5, 'capped at 5');
    p.clear();
    eq(p.liveCount, 0, 'cleared');
    p.destroy();
});

t('particle rate emits particles at expected count', function() {
    var p = scene.createParticles({
        rate: 100,
        maxParticles: 256,
        lifetime: { min: 5, max: 5 },        // long enough not to expire
        velocity: { angle: 0, angleSpread: 0, speed: 0, speedSpread: 0 },
        size: { start: 1, end: 1 },
        color: { start: '#fff', end: '#fff' }
    });
    advanceTime(1000);  // 1s @ 100/s = ~100
    truthy(p.liveCount >= 90 && p.liveCount <= 110,
           'live count near 100 (got ' + p.liveCount + ')');
    p.destroy();
});

t('tilemap setTile/getTile round-trips', function() {
    var m = scene.createTilemap({
        tileWidth: 16, tileHeight: 16,
        columns: 8, rows: 6,
        tileset: { src: '', tileWidth: 16, tileHeight: 16 }
    });
    eq(m.getTile(0, 0), 0, 'empty at start');
    m.setTile(2, 3, 7);
    eq(m.getTile(2, 3), 7, 'roundtrip');
    eq(m.getTile(0, 0), 0, 'other still empty');
    eq(m.getTile(99, 99), 0, 'out-of-bounds returns 0');
    m.destroy();
});

t('tilemap tileAtWorld math', function() {
    var m = scene.createTilemap({
        tileWidth: 32, tileHeight: 32,
        columns: 10, rows: 10,
        x: 0, y: 0,
        tileset: { src: '' }
    });
    var hit = m.tileAtWorld(40, 40);
    truthy(hit, 'in-bounds returns object');
    eq(hit.col, 1);
    eq(hit.row, 1);
    var miss = m.tileAtWorld(-10, -10);
    eq(miss, null, 'out-of-bounds returns null');
    m.destroy();
});

console.log('=== Done: ' + tests + ' tests, ' + failed + ' failures ===');
if (failed > 0) throw new Error(failed + ' tests failed');
