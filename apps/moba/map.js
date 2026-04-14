// map.js — static map data for the MOBA demo.
//
// Coord frame: x east, z south (brogameagent 2D), y is always 0 for gameplay.
// Map is 40x40 centered at origin, with Red on the SW (-x, +z) and Blue on
// the NE (+x, -z). Three lanes connect the nexuses:
//   TOP — along z = -14 (north lane)
//   MID — diagonal through origin
//   BOT — along z = +14 (south lane)
// Nexuses sit in the corners; two towers per lane per side (6 per side, 12
// total — we ship 2 per lane instead of 3 to keep the map legible in the
// ortho view).

const Map = (function () {
    "use strict";

    const BOUNDS = { minX: -20, minZ: -20, maxX: 20, maxZ: 20, cellSize: 0.5 };

    // Nexus positions (Red SW, Blue NE).
    const NEXUS = {
        red:  { x: -17, z:  17 },
        blue: { x:  17, z: -17 },
    };

    // Lane waypoints. Each is a full path from the team's nexus through the
    // lane to the enemy nexus. Minions use the one for their team/lane.
    // Shaped as arcs so minions walk along lanes rather than cutting straight.
    const LANES = {
        red: {
            top: [ { x:-17, z:14 }, { x:-10, z:-14 }, { x:10, z:-14 }, { x:17, z:-14 } ],
            mid: [ { x:-15, z:14 }, { x: -6, z: 6 },  { x:  6, z:-6 }, { x:15, z:-14 } ],
            bot: [ { x:-14, z:17 }, { x:-10, z: 14 }, { x:10, z:14 }, { x:14, z:-14 } ],
        },
        blue: {
            top: [ { x: 17, z:-14 }, { x: 10, z:-14 }, { x:-10, z:-14 }, { x:-17, z:14 } ],
            mid: [ { x: 15, z:-14 }, { x:  6, z: -6 }, { x: -6, z:  6 }, { x:-15, z:14 } ],
            bot: [ { x: 14, z:-14 }, { x: 10, z: 14 }, { x:-10, z:14 }, { x:-14, z:17 } ],
        },
    };

    // Tower positions: two per lane per team, at the first and second
    // defensive rings.
    const TOWERS = [
        // Red (team 0) towers — defending their side
        { team: 0, lane: "top", x: -10, z:-14 },
        { team: 0, lane: "mid", x:  -6, z:  6 },
        { team: 0, lane: "bot", x: -10, z: 14 },
        { team: 0, lane: "top", x:   2, z:-14 },
        { team: 0, lane: "mid", x:   0, z:  0 },
        { team: 0, lane: "bot", x:   2, z: 14 },
        // Blue (team 1) towers — mirrored
        { team: 1, lane: "top", x:  10, z:-14 },
        { team: 1, lane: "mid", x:   6, z: -6 },
        { team: 1, lane: "bot", x:  10, z: 14 },
        { team: 1, lane: "top", x:  -2, z:-14 },
        { team: 1, lane: "mid", x:   0, z:  0 },  // center is shared (two towers at mid)
        { team: 1, lane: "bot", x:  -2, z: 14 },
    ];

    // Jungle walls — impassable chunks off the lanes. Simple AABBs.
    const OBSTACLES = [
        { x: -8, z: 0,  hw: 2, hd: 6 },  // river left
        { x:  8, z: 0,  hw: 2, hd: 6 },  // river right
        { x: 0,  z:-8,  hw: 6, hd: 2 },  // top jungle
        { x: 0,  z: 8,  hw: 6, hd: 2 },  // bot jungle
    ];

    // Minion spawn points (one per lane per team, just outside the nexus).
    const MINION_SPAWN = {
        red:  { top:{x:-16,z:12}, mid:{x:-14,z:12}, bot:{x:-12,z:16} },
        blue: { top:{x: 16,z:-12}, mid:{x: 14,z:-12}, bot:{x: 12,z:-16} },
    };

    return { BOUNDS, NEXUS, LANES, TOWERS, OBSTACLES, MINION_SPAWN };
})();
