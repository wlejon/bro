// First parts-DSL asset: 4-DOF robot arm with a two-finger gripper.
//
// Tests the DSL on a non-trivial assembly:
//   - 8 instances, branching tree (gripper has two fingers)
//   - 5 hinge joints (yaw shoulder, pitch shoulder/elbow/wrist, gripper open)
//   - mirrored finger using `twist: π` to flip the second finger across the
//     palm without authoring two finger parts
//
// Build / capture:
//   bro-headless apps/artstation -e "load('robot_arm'); render(); save('robot_arm');"
//
// In windowed mode, the asset shows up in the picker; transport buttons and
// saveVideo / saveGif work without any extra wiring (defineAssembly compiles
// to a defineScene under the hood).

// ---------- parts -------------------------------------------------------

// Pedestal the arm sits on. Top port is where the shoulder mounts.
definePart('arm_base', {
    build: () => Mesh.cylinder(0.45, 0.18, 24),
    color: '#3a4150',
    metallic: 0.3, roughness: 0.55,
    ports: {
        top: { pos: [0, 0.18, 0], dir: [0, 1, 0], up: [0, 0, 1] },
    },
});

// Joint sphere — used at every articulation point. Has two opposed ports
// so we can chain it inline as the "ball" between two arm segments.
definePart('joint_ball', {
    build: () => Mesh.sphere(0.18, 20, 14),
    color: '#e2b341',
    metallic: 0.85, roughness: 0.25,
    ports: {
        // Two opposed ports along Y. The +Y port mates to the parent
        // (incoming), the -Y port outgoes to the next segment. dir points
        // OUTWARD from the part body in both cases.
        proximal: { pos: [0, 0.18, 0],  dir: [0, 1, 0],  up: [0, 0, 1] },
        distal:   { pos: [0, -0.18, 0], dir: [0, -1, 0], up: [0, 0, 1] },
    },
});

// Long arm segment. Capsule, ports at the two rounded ends.
definePart('arm_segment', {
    build: () => Mesh.capsule(0.14, 0.32, 18, 8),
    color: '#cfd6e0',
    metallic: 0.1, roughness: 0.4,
    ports: {
        // Capsule extends along Y, radius 0.14, halfHeight 0.32 → tip at
        // y = ±(0.32 + 0.14) = ±0.46.
        proximal: { pos: [0, 0.46, 0],  dir: [0, 1, 0],  up: [0, 0, 1] },
        distal:   { pos: [0, -0.46, 0], dir: [0, -1, 0], up: [0, 0, 1] },
    },
});

// Wrist: like joint_ball but with a forward `mount` port for the gripper
// instead of a second axial port. Lets the gripper hang off perpendicular
// to the forearm.
definePart('wrist', {
    build: () => Mesh.sphere(0.16, 18, 12),
    color: '#e2b341',
    metallic: 0.85, roughness: 0.25,
    ports: {
        proximal: { pos: [0, 0.16, 0],  dir: [0, 1, 0],  up: [0, 0, 1] },
        mount:    { pos: [0, -0.16, 0], dir: [0, -1, 0], up: [1, 0, 0] },
    },
});

// Gripper palm. mount mates to the wrist; left/right ports are where the
// two fingers attach. Fingers point outward (perpendicular to mount dir),
// so when closed they sweep TOWARDS each other.
definePart('gripper_palm', {
    build: () => Mesh.box(0.22, 0.09, 0.16),
    color: '#3a4150',
    metallic: 0.3, roughness: 0.55,
    ports: {
        // Box halves: x=±0.22, y=±0.09, z=±0.16. mount on +Y face.
        mount: { pos: [0, 0.09, 0],  dir: [0, 1, 0], up: [0, 0, 1] },
        // Finger ports on the two short ends, pointing outward in -Y so
        // each finger hangs DOWN from the palm. Hinge axis (set per
        // instance) will swing them inward.
        left:  { pos: [-0.22, -0.09, 0], dir: [0, -1, 0], up: [1, 0, 0] },
        right: { pos: [ 0.22, -0.09, 0], dir: [0, -1, 0], up: [1, 0, 0] },
    },
});

// Single finger. proximal at the base (mates to palm), tip at the
// far end (free port — useful as an IK target / muzzle / contact).
definePart('finger', {
    build: () => Mesh.box(0.07, 0.20, 0.10),
    color: '#a23838',
    metallic: 0.2, roughness: 0.45,
    ports: {
        proximal: { pos: [0,  0.20, 0], dir: [0,  1, 0], up: [0, 0, 1] },
        tip:      { pos: [0, -0.20, 0], dir: [0, -1, 0], up: [0, 0, 1] },
    },
});

// ---------- assembly ----------------------------------------------------

defineAssembly('robot_arm', {
    frameWidth: 128, frameHeight: 128,
    fps: 24, duration: 2.0,        // → 48 frames
    cols: 8,                        // 8×6 sheet
    bg: 'transparent',
    pixel: false,                   // 3D — let smoothing through

    camera: {
        fov: 36,
        position: [3.6, 1.8, 3.8],
        target:   [0, 1.0, 0],
        up:       [0, 1, 0],
    },
    lighting: 'studio',
    ambient:  [0.10, 0.10, 0.13],

    parts: {
        // Root. No parent, sits at origin.
        base: { part: 'arm_base' },

        // Shoulder yaw joint — rotates the whole arm about Y. Hinge axis
        // is the world Y, which in base-local coords is also Y.
        shoulder: {
            part: 'joint_ball', parent: 'base',
            via: 'top', at: 'proximal',
            joint: { type: 'hinge', axis: [0, 1, 0] },
        },

        // Upper arm — pitches forward/back. Hinge axis = X in
        // shoulder-local space (perpendicular to the bone). Resting
        // angle leans the upper arm toward the camera.
        upper: {
            part: 'arm_segment', parent: 'shoulder',
            via: 'distal', at: 'proximal',
            joint: { type: 'hinge', axis: [1, 0, 0], angle: 0.5 },
        },

        // Elbow joint sphere. Fixed mount.
        elbow: {
            part: 'joint_ball', parent: 'upper',
            via: 'distal', at: 'proximal',
        },

        // Forearm — pitches at the elbow.
        forearm: {
            part: 'arm_segment', parent: 'elbow',
            via: 'distal', at: 'proximal',
            joint: { type: 'hinge', axis: [1, 0, 0], angle: 1.4 },
        },

        // Wrist sphere — pitches up/down. The frame() callback drives
        // this; the resting -0.3 just gives the static export a nice tilt.
        wristJoint: {
            part: 'wrist', parent: 'forearm',
            via: 'distal', at: 'proximal',
            joint: { type: 'hinge', axis: [1, 0, 0], angle: -0.3 },
        },

        // Gripper palm. Fixed to wrist's mount port.
        palm: {
            part: 'gripper_palm', parent: 'wristJoint',
            via: 'mount', at: 'mount',
        },

        // Two fingers, mirrored. Each hinges about a Z axis in palm-local
        // space, swinging INWARD when the angle is positive. The right
        // finger is mounted with twist=π so its body flips so both
        // fingers are visually identical (heads point the same way).
        fingerL: {
            part: 'finger', parent: 'palm',
            via: 'left', at: 'proximal',
            joint: { type: 'hinge', axis: [0, 0, 1], angle: 0.5 },
        },
        fingerR: {
            part: 'finger', parent: 'palm',
            via: 'right', at: 'proximal',
            joint: { type: 'hinge', axis: [0, 0, -1], angle: 0.5 },  // mirror sign
        },
    },

    frame(refs, t, dt, i) {
        // Two-second loop. The wave is a shoulder-yaw sweep — kept narrow
        // (±0.45 rad ~ ±26°) so the gripper stays inside the 128×128 frame
        // at peak yaw; over the same span the gripper opens/closes once
        // and the wrist nods.
        const yaw   = Math.sin(t * Math.PI)        * 0.45;
        const wrist = -0.4 + Math.sin(t * Math.PI * 2) * 0.20;
        const grip  =  0.25 + 0.55 * (0.5 + 0.5 * Math.cos(t * Math.PI * 2));

        refs._joints.shoulder.angle   = yaw;
        refs._joints.wristJoint.angle = wrist;
        refs._joints.fingerL.angle    = grip;
        refs._joints.fingerR.angle    = grip;
    },

    animations: {
        wave: { frames: 'all', fps: 24, loop: true },
    },
});
