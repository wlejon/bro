/**
 * Animation clips — data-driven multi-track keyframes for scene-node
 * properties. The Godot Animation-resource + AnimationPlayer analog for
 * bro's 3D scene: a clip is plain JSON (storable in files, round-trippable),
 * a player resolves its tracks against named scene nodes and plays it on the
 * engine clock.
 *
 *   const player = scene.createAnimationPlayer();
 *   player.addClip('intro', clipDef);      // plain JSON in
 *   player.play('intro');                  // engine-ticked, zero per-frame JS
 *   player.clipDef('intro');               // the same JSON back out
 *
 * Relationship to the other animation systems:
 *   - Tween (scene.createTween) is imperative "go to X over N seconds";
 *     clips are declarative keyframe data — use clips for anything authored,
 *     stored, or re-played.
 *   - The skeletal player (skinnedMesh.play) drives BONE palettes and is a
 *     separate domain: a clip can animate the same skinned node's
 *     position/rotation/scale and both apply (node TRS vs bone palette).
 *   - CSS/WAAPI animate DOM elements, not scene nodes.
 *
 * Ticking + coexistence (last-writer-wins per property per frame):
 *   Every frame SceneGraph::tickAnimations runs, in order:
 *     1. node ticks (sprite frames, particles, skeletal players)
 *     2. tweens (creation order)
 *     3. clip players (creation order)
 *   so when a tween and a clip player write the same property in the same
 *   frame, the clip player wins; among clip players the newest-created wins.
 *   Everything runs on the scaled clock: bro.time.paused/scale and headless
 *   advanceTime() apply automatically.
 *
 * Liveness: targets are resolved by node NAME at play() time and tracked by
 * node id afterwards — a node destroyed mid-playback simply stops receiving
 * writes (no error, no dangling). Destroying the canvas/scene destroys its
 * players; JS wrappers then throw ("player has been destroyed") on use.
 */

// ── Clip definition (plain JSON) ─────────────────────────────────────────────

const clipDef = {
    duration: 2,            // seconds; optional — defaults to the last key time.
                            // May be shorter than the last key (trailing keys
                            // are then unreachable).
    loop: 'none',           // 'none' (default) — hold final value, fire
                            //   onFinished once
                            // 'loop'     — wrap around
                            // 'pingpong' — reflect at both ends (full cycle
                            //   = 2 x duration)
    tracks: [
        // Property track: target names a scene node (SceneNode.name — first
        // match wins), property one of the supported set below.
        {
            target: 'door',
            property: 'rotation',
            keys: [
                // Keys may be listed in any order (sorted by time on add).
                // interp: interpolation for the segment LEAVING this key —
                //   'linear' (default) — lerp; rotation: shortest-path slerp
                //   'step'             — hold until the next key
                //   'cubic'            — Catmull-Rom: tangents derived from
                //     the neighboring keys (one-sided at the endpoints),
                //     same Hermite basis as the skeletal glTF CUBICSPLINE
                //     path; rotation keys are hemisphere-aligned first
                // ease: any tween easing name ('quadInOut', 'bounceOut', ...)
                //   — warps the segment's normalized time BEFORE the
                //   interpolation above (default 'linear').
                { time: 0.0, value: { euler: [0, 0, 0] }, interp: 'cubic' },
                { time: 1.5, value: { euler: [0, 1.9, 0] }, ease: 'quadOut' },
            ],
        },
        // Event track: named cues fired through player.onEvent when the
        // playhead crosses them. `args` must be JSON-serializable and is
        // delivered by value.
        {
            type: 'event',
            keys: [
                { time: 0.2, name: 'creak', args: { volume: 0.4 } },
                { time: 1.5, name: 'thud' },
            ],
        },
    ],
};

// ── Supported properties ─────────────────────────────────────────────────────
//
// Property/node-type mismatches (and unresolvable target names) are clear
// errors thrown by play(); unknown property names throw at addClip.
//
//   property    value shape          applies to
//   ----------  -------------------  -----------------------------------------
//   position    [x, y, z]            any node
//   rotation    see below            any node (alias: 'quaternion')
//   scale       [x,y,z] or number    any node (number = uniform)
//   opacity     number 0..1          Sprite (opacity), Mesh (color alpha)
//   color       [r,g,b] 0..1 or CSS  Mesh rgb, Light color, Shape fill
//               string ('#ff8800')
//   fov         number, DEGREES      Camera (matches camera.fov)
//   intensity   number               Light
//   range       number               Light
//   emissive    number               Mesh
//   metallic    number               Mesh
//   roughness   number               Mesh
//
// Rotation key values (interpolation is always quaternion slerp — Euler is
// converted on the way in; note plain arrays are quaternions, NOT Euler):
//   { euler: [rx, ry, rz] }          radians, XYZ intrinsic
//   { axis: [x, y, z], angle: a }    axis-angle, radians
//   [x, y, z, w]                     quaternion (normalized on read)
//   1.57                             number = rotation about Z (2D shorthand)

// ── Player ───────────────────────────────────────────────────────────────────

const player = scene.createAnimationPlayer();   // many per scene is fine

player.addClip('intro', clipDef);  // parse + register (replaces same name;
                                   //   an in-flight playback is unaffected)
player.clipDef('intro');           // the verbatim clipDef JSON back out
                                   //   (null for unknown names) — clips are
                                   //   pure data; persist them with
                                   //   JSON.stringify to a file
player.play('intro');              // resolve targets, validate, start
player.play('intro', {
    speed: -1,                     // playback rate; negative = reverse
    from: 0.5,                     // start time (default 0; duration when
                                   //   speed < 0)
    fade: 0.3,                     // crossfade seconds — see below
});
player.pause();                    // freeze in place (fade too)
player.resume();
player.stop();                     // forget the clip: no more writes,
                                   //   currentClip '' / currentTime 0
player.seek(1.2);                  // scrub: clamps to [0, duration], writes
                                   //   immediately (works while paused),
                                   //   fires NO events, clears the finished
                                   //   latch, cancels a running crossfade
player.speed = 2;                  // live-settable mid-playback
player.playing;                    // bool — advancing (not paused/finished)
player.currentTime;                // seconds into the current clip
player.currentClip;                // name, or '' when stopped

player.onFinished = () => {};      // 'none' clips only: fired once when the
                                   //   end (start, in reverse) is reached,
                                   //   after that frame's property writes
player.onEvent = (name, args) => {};  // event-track cues; args is undefined
                                      //   when the key had none
player.destroy();                  // stop + release (safe from callbacks)

// ── Event semantics (Godot-style) ────────────────────────────────────────────
//
//   - A key fires exactly once per pass, when the playhead crosses it —
//     in either direction (reverse playback fires in reverse order).
//   - play() fires a key sitting exactly at the start position; each loop
//     pass re-fires keys (including one at t=0); pingpong fires boundary
//     keys once per arrival.
//   - seek() NEVER fires events — not the skipped ones, not the key at the
//     seek position itself. Keys strictly ahead of the playhead fire as
//     playback proceeds.
//   - Callbacks run during the engine tick, before that frame's property
//     writes. Calling play/stop/seek/destroy on the player from inside a
//     callback is safe (the in-flight tick aborts cleanly).
//
// ── Crossfade semantics ──────────────────────────────────────────────────────
//
// player.play(next, { fade: seconds }) blends from the current state:
//   - The outgoing clip keeps advancing during the fade — property tracks
//     only; its EVENT tracks stop firing at the switch.
//   - Each incoming property track blends toward its sampled value from:
//     the outgoing clip's matching (same node, same property) track when
//     one exists, else the property's actual value captured at the switch.
//     Rotation blends by slerp, everything else lerps; the weight ramps
//     linearly over `fade` seconds (real scaled time, unaffected by either
//     clip's speed).
//   - Outgoing tracks nothing blends against keep writing until the fade
//     ends, then freeze in place.
//   - Starting a new fade mid-fade drops the older fade source (snap);
//     seek() and stop() cancel the fade.

// ── Example: two-node cutscene ───────────────────────────────────────────────
//
// A door swings open while the room light dims to amber; a cue fires a
// creak sound at the moment the door starts moving.

const door = scene.createMesh({ name: 'door' });      // hinged at its origin
const lamp = scene.createLight({ name: 'lamp', type: 'point',
                                 color: [1, 1, 1], intensity: 20 });

const cutscene = scene.createAnimationPlayer();
cutscene.addClip('doorOpen', {
    duration: 2.5,
    tracks: [
        { target: 'door', property: 'rotation', keys: [
            { time: 0.4, value: { euler: [0, 0, 0] }, ease: 'quadIn' },
            { time: 2.0, value: { euler: [0, -1.9, 0] }, ease: 'backOut' },
        ]},
        { target: 'lamp', property: 'intensity', keys: [
            { time: 0.0, value: 20 },
            { time: 2.5, value: 6, ease: 'sineInOut' },
        ]},
        { target: 'lamp', property: 'color', keys: [
            { time: 0.0, value: [1, 1, 1] },
            { time: 2.5, value: '#ffb060' },
        ]},
        { type: 'event', keys: [
            { time: 0.4, name: 'sfx', args: { src: 'creak.ogg' } },
        ]},
    ],
});

const actx = new AudioContext();
const dec = actx.decodeAudioFile('creak.ogg');
const sfx = { 'creak.ogg': actx.createClip(dec.samples, dec.channels,
                                           dec.sampleRate) };
cutscene.onEvent = (name, args) => {
    if (name === 'sfx') actx.playClip(sfx[args.src]);
};
cutscene.onFinished = () => console.log('cutscene done');
cutscene.play('doorOpen');

// Because clips are plain JSON, the cutscene can live in a file:
//   const fs = require('fs');
//   fs.writeFileSync('cutscene.json',
//                    JSON.stringify(cutscene.clipDef('doorOpen')));
//   cutscene.addClip('doorOpen',
//                    JSON.parse(fs.readFileSync('cutscene.json', 'utf-8')));


// ═════════════════════════════════════════════════════════════════════════════
// Skeletal blending — N-way blend spaces + layered blending (SkinnedMeshNode)
// ═════════════════════════════════════════════════════════════════════════════
//
// The skinned-mesh player (skinnedMesh.setSkeleton / addClip / play — method
// reference in docs/scene-api.js) blends BONE poses, entirely in C++, with
// zero per-frame JS. Beyond single clips and crossfades it supports:
//
//   - BLEND SPACES (Godot BlendSpace1D/2D analog): named sets of clips at
//     parameter positions; one scalar (1D) or 2D point picks the mix.
//   - LAYERS: up to 8 ordered masked layers over the base (slot 0..7),
//     each with a runtime weight and its own fade in/out.
//
// Everything below runs on the engine clock (headless advanceTime() works)
// and allocates nothing per frame.
//
// ── Blend spaces ─────────────────────────────────────────────────────────────
//
//   node.addBlendSpace1D('locomotion', [
//       { clip: 'idle', pos: 0 },
//       { clip: 'walk', pos: 2 },            // m/s at which walk looks right
//       { clip: 'run',  pos: 6, timescale: 1.1 },
//   ]);
//   node.play('locomotion', { fadeTime: 0.25 });   // a base-track citizen
//   node.setBlendPos('locomotion', 3.1);           // instant; clamped to [0,6]
//
//   node.addBlendSpace2D('strafe', [
//       { clip: 'walkF', pos: [0,  1] },
//       { clip: 'walkB', pos: [0, -1] },
//       { clip: 'walkL', pos: [-1, 0] },
//       { clip: 'walkR', pos: [ 1, 0] },
//       { clip: 'idle',  pos: [0,  0] },
//   ]);
//   node.play('strafe');
//   node.setBlendPos('strafe', vx, vz);            // or setBlendPos(name, [x, z])
//
// Semantics:
//   - Clips must be registered via addClip() first; the space captures them
//     at addBlendSpace time. Re-adding a space with the same name replaces
//     it in place (safe while playing).
//   - play(name) treats blend spaces exactly like clips on the BASE track:
//     fadeTime crossfades into/out of a space through the normal fade
//     machinery, and spaces shadow a same-named clip. Layers compose on
//     top unchanged. Blend spaces always loop.
//   - 1D: the parameter clamps to [min pos, max pos]; the two neighboring
//     clips blend linearly by position (exact slerp — at a sample point you
//     get that clip bit-exactly).
//   - 2D: the 3 nearest points blend by inverse-SQUARED-distance weights,
//     normalized. On a sample point that clip takes full weight (coincident
//     points split it evenly); degenerate layouts (duplicate or collinear
//     points) are safe. This is deliberately simpler than Godot's
//     triangulated BlendSpace2D: no triangulation to author or break, but
//     weights jump slightly when the nearest-3 set changes — keep sample
//     points sparse and well-separated.
//   - setBlendPos is INSTANT — no internal smoothing. Tween the parameter
//     from app code for eased transitions; the coming state-machine tier
//     will add authored transitions on top.
//
// ── Phase sync ───────────────────────────────────────────────────────────────
//
// All clips in a playing space advance on ONE shared normalized phase
// (0..1 of each clip's own duration), so a 1.0 s walk and a 0.6 s run stay
// foot-aligned at any mix. The blended cycle duration is the weight-mixed
// duration of the participating clips (Σ wᵢ · durᵢ / timescaleᵢ); the
// phase advances at speed / that duration. Per-clip `timescale` compensates
// authored cadence differences (2 = this clip represents a half-length
// cycle) without resampling the clip.
//
// ── Layers ───────────────────────────────────────────────────────────────────
//
//   node.playLayer(1, 'wave',  { mask: upperBody, weight: 0.8 });
//   node.playLayer(2, 'blink', { mask: faceBones, fadeTime: 0.2 });
//   node.setLayerWeight(1, 0.5);                   // instant runtime weight
//   node.stopLayer(1, { fadeTime: 0.3 });          // fade out, then free slot
//
//   - 8 slots (0..7), blended over the base in ascending slot order.
//     playLayer replaces its slot atomically; the legacy
//     play(name, { mask }) form is slot 0.
//   - mask: Uint8Array/array, 1 = bone animated by this layer; empty/omitted
//     = whole body. fadeTime on playLayer fades the layer's WEIGHT in from
//     0 (layers never crossfade — that's a base-track concept).
//   - A non-looping layer expires on finish and fires onAnimationFinished
//     with its clip name; looping layers persist until stopLayer/stop.
//   - Layers play clips only; blend spaces live on the base track.
//
// ── Introspection ────────────────────────────────────────────────────────────
//
//   node.blendState() → {
//     clips:  [{ name, weight }],  // base composition; weights sum to 1
//                                  // (during a crossfade the outgoing source
//                                  //  appears too, scaled by 1 - alpha)
//     phase:  0.42,                // space: shared phase 0..1; clip: t/dur
//     pos:    [x] | [x, y],        // present while a space is the base
//     layers: [{ slot, name, weight, phase }],   // active, ascending slot
//   }
//
// Cheap enough for HUDs and assertions; tests drive it with advanceTime().
//
// ── Recipe: speed-driven locomotion ──────────────────────────────────────────

const char2 = scene.createSkinnedMesh({ data: gltf.meshes[0], skin: gltf.skins[0] });
char2.setSkeleton(gltf.skeletons[0]);
char2.addClip('idle', gltf.animations[0]);
char2.addClip('walk', gltf.animations[1]);
char2.addClip('run',  gltf.animations[2]);

char2.addBlendSpace1D('locomotion', [
    { clip: 'idle', pos: 0 },
    { clip: 'walk', pos: 2 },
    { clip: 'run',  pos: 6 },
]);
char2.play('locomotion', { fadeTime: 0.2 });

// Each frame: feed the character's actual planar speed into the space.
// The parameter is instant, so smooth it from gameplay code (or tween it).
let smoothSpeed = 0;
function onTick(dt, velocity) {
    const speed = Math.hypot(velocity[0], velocity[2]);
    smoothSpeed += (speed - smoothSpeed) * Math.min(1, dt * 8);
    char2.setBlendPos('locomotion', smoothSpeed);
}

// An upper-body action over the moving base, whatever the current mix:
// char2.playLayer(1, 'wave', { mask: upperBody, fadeTime: 0.15 });
