// =============================================================================
// Follow-me tool — sweep a profile face along a path.
//
// Two-stage state machine:
//   stage 'pick-profile' — first click: a face on a primitive becomes the
//                          profile (boundary loop in world space + face
//                          plane normal).
//   stage 'pick-path'    — second click: an EdgePrimitive is the path. The
//                          tube is built immediately and committed as a
//                          new primitive.
//
// Output: a new face primitive (triangulated tube). The source profile face
// is left intact. Ring orientations propagate via min-rotation between
// successive segment directions (parallel transport), with bisector miters
// at interior path vertices to keep adjacent quads coplanar.
// =============================================================================

(function (global) {
    'use strict';

    function createState() {
        return {
            stage:         'idle',
            profileLoop:   null,        // 3D world-space loop
            profileNormal: null,        // unit normal (world)
            profileSource: null,        // source primitive (for picker exclusion)
            profileGroup:  -1,
        };
    }

    function active(state) { return state.stage !== 'idle'; }

    // Begin: arm with a profile. Caller supplies the boundary loop and the
    // face normal in WORLD space. We store identifying info in case the app
    // wants to surface a hint or exclude the source from the path picker.
    function beginWithProfile(state, primitive, groupIdx, loopWorld, normalWorld) {
        state.stage         = 'pick-path';
        state.profileSource = primitive;
        state.profileGroup  = groupIdx;
        state.profileLoop   = loopWorld.map(p => p.slice());
        state.profileNormal = normalWorld.slice();
    }

    // Commit with a path (array of [x,y,z] world points). Returns
    // { positions, indices, normals } or null.
    function commitWithPath(state, pathWorld) {
        if (state.stage !== 'pick-path') return null;
        if (!pathWorld || pathWorld.length < 2) {
            clear(state);
            return null;
        }
        const mesh = Sketch.sweepProfile(
            state.profileLoop, state.profileNormal, pathWorld);
        clear(state);
        return mesh;
    }

    function cancel(state) { clear(state); }

    function clear(state) {
        state.stage         = 'idle';
        state.profileSource = null;
        state.profileGroup  = -1;
        state.profileLoop   = null;
        state.profileNormal = null;
    }

    global.FollowMeTool = {
        createState, active, beginWithProfile, commitWithPath, cancel,
    };

})(typeof globalThis !== 'undefined' ? globalThis : this);
