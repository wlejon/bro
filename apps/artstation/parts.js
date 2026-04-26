// Parts DSL: compose 3D meshes by mating named ports on rigid pieces.
//
// A "part" is pure data — a mesh-builder + a set of named ports (local-space
// frames). An "assembly" is a tree of part instances connected port-to-port.
// Mating two ports puts the child at the parent port's world position with
// its outward direction flipped (the ports face each other).
//
// Output today is a tree of plain MeshNodes in the scene — one per
// instance. No GPU skinning yet; per-frame the assembly recomputes world
// transforms from the joint state, which is plenty fast for the
// sprite-sheet capture path defineScene already uses.
//
// Why parts and not nested scenes:
//   - A part is a value (referentially transparent). Reuse the same
//     fingertip 10 places without identity collisions.
//   - The output projection (unified mesh + skeleton, eventually) is a
//     flat structure. Part-graph projects onto it cleanly.
//   - Authoring DSL ≠ runtime composition. Nested scenes will still earn
//     their keep later for security-camera-feed style needs.
//
// See README.md for the docs version of this.

(function () {
    'use strict';

    const PARTS = {};
    const ASSEMBLIES = {};

    // ---- definePart ----------------------------------------------------
    // spec: {
    //   build: () => Mesh | MeshData,         // bromesh Mesh handle
    //   ports: { name: { pos, dir, up? }, ... }  // local-space frames
    //   color?, metallic?, roughness?, emissive?, emissiveColor?,
    //   scale?,  // baked into mesh-build (won't affect ports)
    //   transform?,  // (mesh) => mesh hook for CSG / shrinkwrap / etc
    // }
    //
    // Port frames live in the part's local space. `pos` is the attach
    // point. `dir` is the OUTWARD direction (away from the part body) —
    // when mated, two ports face each other so the child's `dir` points
    // back into the parent. `up` (optional) pins twist.
    function definePart(name, spec) {
        if (!spec || typeof spec.build !== 'function') {
            throw new Error(`definePart('${name}') requires build()`);
        }
        const ports = spec.ports || {};
        for (const [pname, p] of Object.entries(ports)) {
            if (!Array.isArray(p.pos) || p.pos.length !== 3) {
                throw new Error(`part ${name} port ${pname}: pos must be [x,y,z]`);
            }
            if (!Array.isArray(p.dir) || p.dir.length !== 3) {
                throw new Error(`part ${name} port ${pname}: dir must be [x,y,z]`);
            }
        }
        PARTS[name] = spec;
    }

    // ---- defineAssembly ------------------------------------------------
    // spec: {
    //   parts: {
    //     // Root has no parent.
    //     instanceName: { part: 'partName', ...overrides },
    //     // Children declare parent + ports they mate with.
    //     childName: {
    //       part: 'otherPart',
    //       parent: 'instanceName',
    //       via: 'parentPort',     // port on parent
    //       at:  'childPort',      // port on child
    //       joint?: { type: 'fixed' | 'hinge', axis?: [x,y,z] },
    //       twist?: 0,             // initial twist (radians) about port axis
    //     },
    //   },
    //   // Optional. Setup once per render before frames start.
    //   setup?(refs, scene) {},
    //   // Optional. Drive joint angles per frame.
    //   frame?(refs, t, dt, i) {},
    //
    //   // Capture metadata, same shape as defineScene.
    //   frameWidth, frameHeight, fps, duration, cols, bg, pixel,
    //   camera?, lighting?, ambient?, animations?,
    // }
    function defineAssembly(name, spec) {
        if (!spec || !spec.parts) {
            throw new Error(`defineAssembly('${name}') requires parts {}`);
        }
        ASSEMBLIES[name] = spec;

        // Compile to a defineScene under the hood so render / save /
        // saveVideo / saveGif / preview all flow through the same pipeline
        // we already use for 3D sprite-sheet capture. Asset code stays
        // declarative; the framework owns the build/frame plumbing.
        if (typeof window.defineScene !== 'function') {
            throw new Error('defineAssembly: app.js must load before parts.js');
        }

        const compiled = {
            frameWidth:  spec.frameWidth  || 128,
            frameHeight: spec.frameHeight || 128,
            fps:         spec.fps         || 24,
            duration:    spec.duration    || 1.0,
            cols:        spec.cols        || 8,
            bg:          spec.bg          || 'transparent',
            pixel:       spec.pixel === undefined ? false : spec.pixel,
            animations:  spec.animations,

            build(scene) {
                if (spec.lighting === false) {
                    // Asset opted out of presets — let setup() own lighting.
                } else {
                    applyLightingPreset(scene, spec.lighting || 'studio',
                                        spec.ambient);
                }
                if (spec.camera) {
                    scene.setCamera(spec.camera);
                } else {
                    scene.setCamera({
                        fov: 35,
                        position: [3.5, 2.5, 4.5],
                        target:   [0, 0.4, 0],
                        up:       [0, 1, 0],
                    });
                }

                const refs = instantiate(scene, name);
                if (typeof spec.setup === 'function') {
                    spec.setup(refs, scene);
                }
                // Re-resolve world transforms after setup() in case it
                // mutated initial joint angles or root pose.
                updateAssembly(refs);
                return refs;
            },
            frame(scene, t, dt, refs, i) {
                if (typeof spec.frame === 'function') {
                    spec.frame(refs, t, dt, i);
                }
                updateAssembly(refs);
            },
        };
        window.defineScene(name, compiled);
    }

    // Lighting presets — small set of named studio rigs so the common
    // case is one-liner.
    function applyLightingPreset(scene, preset, ambientOverride) {
        const ambient = ambientOverride || [0.10, 0.10, 0.12];
        scene.setAmbient(ambient);
        switch (preset) {
            case 'studio':
                scene.createLight({
                    type: 'directional',
                    direction: [-0.5, -1.0, -0.3],
                    color: [1.0, 0.97, 0.92],
                    intensity: 2.4,
                });
                scene.createLight({
                    type: 'directional',
                    direction: [0.6, -0.4, 0.7],
                    color: [0.7, 0.8, 1.0],
                    intensity: 0.9,
                });
                break;
            case 'sun':
                scene.createLight({
                    type: 'directional',
                    direction: [-0.3, -1.0, -0.2],
                    color: [1.0, 0.95, 0.85],
                    intensity: 3.0,
                });
                break;
            case 'dramatic':
                scene.createLight({
                    type: 'directional',
                    direction: [-0.8, -0.6, -0.2],
                    color: [1.0, 0.85, 0.7],
                    intensity: 3.5,
                });
                break;
            default:
                throw new Error(`unknown lighting preset '${preset}'`);
        }
    }

    function listParts()      { return Object.keys(PARTS); }
    function listAssemblies() { return Object.keys(ASSEMBLIES); }

    // ---- vec/quat math --------------------------------------------------
    // Quaternions are [x,y,z,w], same convention as the scene API.
    // Vectors are [x,y,z]. Everything is plain arrays — keeps the DSL
    // hot-reloadable without holding onto opaque handles.

    function vlen(a)        { return Math.hypot(a[0], a[1], a[2]); }
    function vnorm(a) {
        const l = vlen(a) || 1;
        return [a[0]/l, a[1]/l, a[2]/l];
    }
    function vneg(a) { return [-a[0], -a[1], -a[2]]; }
    function vdot(a, b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
    function vcross(a, b) {
        return [
            a[1]*b[2] - a[2]*b[1],
            a[2]*b[0] - a[0]*b[2],
            a[0]*b[1] - a[1]*b[0],
        ];
    }
    function vadd(a, b) { return [a[0]+b[0], a[1]+b[1], a[2]+b[2]]; }
    function vsub(a, b) { return [a[0]-b[0], a[1]-b[1], a[2]-b[2]]; }
    function vmul(a, s) { return [a[0]*s, a[1]*s, a[2]*s]; }

    const QID = [0, 0, 0, 1];

    function qnorm(q) {
        const l = Math.hypot(q[0], q[1], q[2], q[3]) || 1;
        return [q[0]/l, q[1]/l, q[2]/l, q[3]/l];
    }
    function qmul(a, b) {
        // Hamilton product; xyzw layout.
        const ax = a[0], ay = a[1], az = a[2], aw = a[3];
        const bx = b[0], by = b[1], bz = b[2], bw = b[3];
        return [
            aw*bx + ax*bw + ay*bz - az*by,
            aw*by - ax*bz + ay*bw + az*bx,
            aw*bz + ax*by - ay*bx + az*bw,
            aw*bw - ax*bx - ay*by - az*bz,
        ];
    }
    // Rotate vector v by quaternion q.
    function qrot(q, v) {
        const qv = [q[0], q[1], q[2]];
        const t = vmul(vcross(qv, v), 2);
        return vadd(vadd(v, vmul(t, q[3])), vcross(qv, t));
    }
    function qFromAxisAngle(axis, angle) {
        const a = vnorm(axis);
        const s = Math.sin(angle * 0.5);
        return [a[0]*s, a[1]*s, a[2]*s, Math.cos(angle * 0.5)];
    }
    // Shortest rotation that maps unit vector `from` onto unit vector `to`.
    // Standard "half-vector" formulation; handles the 180° case by picking
    // an arbitrary perpendicular axis.
    function qFromTo(from, to) {
        const f = vnorm(from);
        const t = vnorm(to);
        const d = vdot(f, t);
        if (d > 0.999999) return [0, 0, 0, 1];
        if (d < -0.999999) {
            // Pick any axis perpendicular to f.
            let axis = vcross(f, [1, 0, 0]);
            if (vlen(axis) < 1e-4) axis = vcross(f, [0, 1, 0]);
            return qFromAxisAngle(vnorm(axis), Math.PI);
        }
        const h = vnorm(vadd(f, t));
        const xyz = vcross(f, h);
        return qnorm([xyz[0], xyz[1], xyz[2], vdot(f, h)]);
    }

    // ---- port mating ---------------------------------------------------
    // Given a parent port (already in world space) and a child port
    // (still in child-local space), compute the child's world position +
    // orientation so the two ports mate: positions coincide, child.dir
    // points opposite to parent.dir, optional up vectors align (if both
    // have an up), plus an explicit `twist` rotation about the joint axis.
    //
    // Math:
    //   1. Find quaternion Qbase that rotates child.dir → -parent.dir.
    //   2. If both ports specify `up`, add a roll Qroll about the now-aligned
    //      joint axis so child's up (rotated) lines up with parent's up
    //      (negated, since the child is mounted facing into the parent).
    //   3. Combine with a user `twist` about the joint axis.
    //   4. Position the child so Qfinal*child.pos + childWorld == parent.pos.
    function matePorts(parentPortWorldPos, parentPortWorldDir, parentPortWorldUp,
                       childPort, twist) {
        const targetDir = vneg(parentPortWorldDir);

        // Step 1: align child.dir → -parent.dir.
        let q = qFromTo(childPort.dir, targetDir);

        // Step 2: roll. parentUp must be made perpendicular to targetDir
        // (project onto the plane normal to targetDir) before measuring.
        if (childPort.up && parentPortWorldUp) {
            const childUpRotated = qrot(q, childPort.up);
            // Parent's "up after mating" is its own up vector (already in
            // world space). Both should be perpendicular to targetDir; we
            // rotate around targetDir to align them.
            const axis = targetDir;
            const u1 = vsub(childUpRotated, vmul(axis, vdot(childUpRotated, axis)));
            const u2 = vsub(parentPortWorldUp,  vmul(axis, vdot(parentPortWorldUp,  axis)));
            const lu1 = vlen(u1), lu2 = vlen(u2);
            if (lu1 > 1e-4 && lu2 > 1e-4) {
                const a = [u1[0]/lu1, u1[1]/lu1, u1[2]/lu1];
                const b = [u2[0]/lu2, u2[1]/lu2, u2[2]/lu2];
                let cos = vdot(a, b); if (cos > 1) cos = 1; if (cos < -1) cos = -1;
                const sin = vdot(vcross(a, b), axis);
                const angle = Math.atan2(sin, cos);
                q = qmul(qFromAxisAngle(axis, angle), q);
            }
        }

        // Step 3: explicit twist about the joint axis.
        if (twist) {
            q = qmul(qFromAxisAngle(targetDir, twist), q);
        }

        // Step 4: position. childWorld + q*childPort.pos = parentPort.pos.
        const childPosRot = qrot(q, childPort.pos);
        const pos = vsub(parentPortWorldPos, childPosRot);

        return { pos, quat: qnorm(q) };
    }

    // Transform a port from a part's local space to world space, given the
    // part's own world placement (pos + quat).
    function portWorld(partPos, partQuat, port) {
        return {
            pos: vadd(partPos, qrot(partQuat, port.pos)),
            dir: qrot(partQuat, port.dir),
            up:  port.up ? qrot(partQuat, port.up) : null,
        };
    }

    // ---- assembly instantiation ----------------------------------------
    // Create one MeshNode per instance, place it at the identity at first.
    // The actual transforms are computed in updateAssembly() below — that
    // way the same path runs every frame after joint angles change.
    function instantiate(scene, name) {
        const spec = ASSEMBLIES[name];
        if (!spec) throw new Error(`assembly '${name}' not loaded`);

        // Resolve build order: parents before children.
        const order = topoOrder(spec.parts);
        const refs = { _spec: spec, _order: order, _nodes: {}, _joints: {} };

        for (const inst of order) {
            const decl = spec.parts[inst];
            const part = PARTS[decl.part];
            if (!part) throw new Error(`assembly '${name}' references unknown part '${decl.part}'`);

            // Build the mesh once. Re-use the same MeshData for every
            // instance of the same part — but we still create one MeshNode
            // per instance (different transforms / materials per instance).
            const mesh = part.build();
            const node = scene.createMesh({
                data: mesh,
                color:     decl.color     || part.color,
                metallic:  decl.metallic  != null ? decl.metallic  : part.metallic,
                roughness: decl.roughness != null ? decl.roughness : part.roughness,
                emissive:  decl.emissive  != null ? decl.emissive  : part.emissive,
                emissiveColor: decl.emissiveColor || part.emissiveColor,
                name: inst,
            });

            // Joint state lives on refs so spec.frame() can mutate it
            // without grovelling through the assembly internals.
            //   twist : constant initial roll about the port outward dir
            //           (for mount orientation — fingers around a palm, etc).
            //   angle : dynamic hinge angle in radians (for type:'hinge').
            //   axis  : hinge axis in PARENT-PART-LOCAL coords. Defaults to
            //           the parent port's outward dir (twist hinge). Use
            //           [1,0,0] etc. for elbow-style perpendicular hinges.
            // Initial hinge angle can come from decl.joint.angle so the
            // resting pose stays declarative — no need to grovel through
            // setup() just to hold a static fold. spec.setup() is still
            // allowed (and useful for poses that depend on shared state).
            const j = decl.joint || { type: 'fixed' };
            refs._joints[inst] = {
                twist: decl.twist || 0,
                angle: (j.angle != null) ? j.angle : 0,
                joint: j,
            };
            refs._nodes[inst] = node;
            refs[inst] = node;
        }

        updateAssembly(refs);
        return refs;
    }

    // Walk the part graph in parent-before-child order so each child can
    // resolve its parent's world transform when computing its own.
    function topoOrder(parts) {
        const seen = new Set();
        const out = [];
        function visit(name) {
            if (seen.has(name)) return;
            const decl = parts[name];
            if (!decl) throw new Error(`assembly references undefined instance '${name}'`);
            if (decl.parent) visit(decl.parent);
            seen.add(name);
            out.push(name);
        }
        for (const name of Object.keys(parts)) visit(name);
        return out;
    }

    // Recompute every node's world transform from joint state. Called once
    // at instantiate-time and again after every frame() callback so joint
    // mutations propagate.
    function updateAssembly(refs) {
        const spec  = refs._spec;
        const order = refs._order;
        // Cache each instance's resolved world transform so children can
        // look up their parent's port-world frames.
        const worldT = {};

        for (const inst of order) {
            const decl = spec.parts[inst];
            const part = PARTS[decl.part];

            if (!decl.parent) {
                // Root — place at origin (or explicit decl.pos / decl.quat).
                const pos = decl.pos || [0, 0, 0];
                const quat = decl.quat || QID;
                worldT[inst] = { pos, quat };
                refs._nodes[inst].x = pos[0];
                refs._nodes[inst].y = pos[1];
                refs._nodes[inst].z = pos[2];
                refs._nodes[inst].quaternion = quat;
                continue;
            }

            const parentT = worldT[decl.parent];
            const parentPart = PARTS[spec.parts[decl.parent].part];
            const parentPort = parentPart.ports[decl.via];
            if (!parentPort) {
                throw new Error(
                    `instance '${inst}': parent '${decl.parent}' has no port '${decl.via}'`);
            }
            const childPort = part.ports[decl.at];
            if (!childPort) {
                throw new Error(
                    `instance '${inst}': part '${decl.part}' has no port '${decl.at}'`);
            }

            const ppw = portWorld(parentT.pos, parentT.quat, parentPort);

            // Apply hinge: rotate the parent port's outward frame about the
            // hinge axis (axis lives in parent-part-local coords; world axis
            // = parent's quat rotated). Pos stays put — hinge passes through
            // the port. matePorts then mates the child to the *rotated*
            // parent frame.
            const j = refs._joints[inst];
            let pdir = ppw.dir, pup = ppw.up;
            if (j.joint && j.joint.type === 'hinge' && j.angle) {
                const axisLocal = j.joint.axis || parentPort.dir;
                const axisWorld = qrot(parentT.quat, axisLocal);
                const hingeQ = qFromAxisAngle(axisWorld, j.angle);
                pdir = qrot(hingeQ, pdir);
                if (pup) pup = qrot(hingeQ, pup);
            }

            const mate = matePorts(ppw.pos, pdir, pup, childPort, j.twist);
            worldT[inst] = mate;

            const node = refs._nodes[inst];
            node.x = mate.pos[0];
            node.y = mate.pos[1];
            node.z = mate.pos[2];
            node.quaternion = mate.quat;
        }
        refs._worldT = worldT;
    }

    // Look up a port's *current* world frame after an updateAssembly().
    // Useful for frame() callbacks that want to attach ad-hoc effects to a
    // port (muzzle flash, IK targets) without baking another part there.
    function getPortWorld(refs, instance, portName) {
        const decl = refs._spec.parts[instance];
        if (!decl) return null;
        const part = PARTS[decl.part];
        const port = part.ports[portName];
        if (!port) return null;
        const wt = refs._worldT[instance];
        return portWorld(wt.pos, wt.quat, port);
    }

    // ---- exports -------------------------------------------------------
    // app.js wires these onto the global object during init so asset
    // modules (which are eval'd into global scope) can call them.
    window.definePart       = definePart;
    window.defineAssembly   = defineAssembly;
    window.__partsRegistry  = {
        parts:        PARTS,
        assemblies:   ASSEMBLIES,
        instantiate,
        updateAssembly,
        getPortWorld,
        listParts,
        listAssemblies,
        // Math helpers for asset code that wants to write its own
        // port-space transformations without re-deriving them.
        math: { vadd, vsub, vmul, vnorm, vcross, vdot,
                qmul, qrot, qFromAxisAngle, qFromTo, qnorm },
    };
})();
