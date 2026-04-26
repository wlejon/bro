// "terrain" — 16x16 tileset for top-down maps.
// Tile 0 is reserved (engine treats 0 as empty), so the layout is:
//   1 grass, 2 dirt, 3 stone, 4 water, 5 sand,
//   6 grass+flowers, 7 tree, 8 wall

const P = brush.ENDESGA16;
const GRASS_LIGHT = '#63c64d';   // P[8]
const GRASS_DARK  = '#327345';   // P[9]
const GRASS_DEEP  = '#193d3f';   // P[10]
const DIRT_LIGHT  = '#e4a672';   // P[0]
const DIRT_DARK   = '#b86f50';   // P[1]
const DIRT_DEEP   = '#743f39';   // P[2]
const STONE_LIGHT = '#afbfd2';   // P[12]
const STONE_DARK  = '#4f6781';   // P[11]
const WATER_LIGHT = '#2ce8f4';   // P[14]
const WATER_DARK  = '#0484d1';   // P[15]
const SAND        = '#ffe762';   // P[7]
const FLOWER      = '#fb922b';   // P[6]
const FLOWER_RED  = '#e53b44';   // P[5]
const TREE_TRUNK  = '#743f39';
const TREE_LEAF   = '#327345';
const TREE_LEAF_H = '#63c64d';
const WALL_LIGHT  = '#5f574f';
const WALL_DARK   = '#3f2832';

// Hash-based dithering so the same tile always renders the same speckle
// pattern (deterministic, no rng).  s = tileSize.
function speckle(ctx, s, color, density) {
    ctx.fillStyle = color;
    for (let y = 0; y < s; y++) {
        for (let x = 0; x < s; x++) {
            const h = ((x * 374761393) ^ (y * 668265263)) >>> 0;
            if ((h % 100) < density) ctx.fillRect(x, y, 1, 1);
        }
    }
}

defineTileset('terrain', {
    tileSize: 16,
    cols: 8,
    bg: 'transparent',
    tiles: [
        null, // index 0 = empty (engine convention)

        // 1: GRASS — base + sparse darker speckle + a few light highlights
        (ctx, s) => {
            brush.rect(ctx, 0, 0, s, s, GRASS_LIGHT);
            speckle(ctx, s, GRASS_DARK, 18);
            speckle(ctx, s, GRASS_DEEP, 6);
        },

        // 2: DIRT — warm earth, mottled
        (ctx, s) => {
            brush.rect(ctx, 0, 0, s, s, DIRT_LIGHT);
            speckle(ctx, s, DIRT_DARK, 22);
            speckle(ctx, s, DIRT_DEEP, 8);
        },

        // 3: STONE — cobble pattern (4 quadrants with offset highlights)
        (ctx, s) => {
            brush.rect(ctx, 0, 0, s, s, STONE_DARK);
            const half = s / 2;
            // four cobblestones with light tops
            for (let qy = 0; qy < 2; qy++) {
                for (let qx = 0; qx < 2; qx++) {
                    const x = qx * half, y = qy * half;
                    brush.rect(ctx, x + 1, y + 1, half - 2, half - 2, STONE_LIGHT);
                    brush.hline(ctx, x + 1, y + 1, half - 2, '#d4dce8');
                    brush.vline(ctx, x + 1, y + 1, half - 2, '#d4dce8');
                }
            }
        },

        // 4: WATER — base + horizontal "ripple" lines at 2 rows
        (ctx, s) => {
            brush.rect(ctx, 0, 0, s, s, WATER_DARK);
            speckle(ctx, s, WATER_LIGHT, 12);
            brush.hline(ctx, 2, 4, 5, WATER_LIGHT);
            brush.hline(ctx, 9, 11, 5, WATER_LIGHT);
        },

        // 5: SAND — soft yellow with subtle dirt fleck
        (ctx, s) => {
            brush.rect(ctx, 0, 0, s, s, SAND);
            speckle(ctx, s, DIRT_LIGHT, 14);
        },

        // 6: GRASS WITH FLOWERS — same base + a couple of flower clusters
        (ctx, s) => {
            brush.rect(ctx, 0, 0, s, s, GRASS_LIGHT);
            speckle(ctx, s, GRASS_DARK, 18);
            // two flower stems with petals
            brush.px(ctx, 4, 8, FLOWER);
            brush.px(ctx, 3, 7, FLOWER); brush.px(ctx, 5, 7, FLOWER);
            brush.px(ctx, 4, 6, FLOWER);
            brush.px(ctx, 11, 12, FLOWER_RED);
            brush.px(ctx, 10, 11, FLOWER_RED); brush.px(ctx, 12, 11, FLOWER_RED);
            brush.px(ctx, 11, 10, FLOWER_RED);
        },

        // 7: TREE — grass base, dark trunk, round canopy
        (ctx, s) => {
            brush.rect(ctx, 0, 0, s, s, GRASS_LIGHT);
            speckle(ctx, s, GRASS_DARK, 12);
            // trunk
            brush.rect(ctx, 7, 10, 2, 4, TREE_TRUNK);
            // canopy (filled circle)
            brush.circle(ctx, 8, 6, 5, TREE_LEAF);
            // highlights on top-left of canopy
            brush.px(ctx, 6, 4, TREE_LEAF_H);
            brush.px(ctx, 7, 3, TREE_LEAF_H);
            brush.px(ctx, 5, 5, TREE_LEAF_H);
        },

        // 8: WALL — brick pattern with offset rows
        (ctx, s) => {
            brush.rect(ctx, 0, 0, s, s, WALL_DARK);
            // Row of bricks (top half) — bricks 5w x 3h with 1px mortar
            for (let r = 0; r < 4; r++) {
                const y = r * 4;
                const offset = (r % 2) * 4;
                for (let x = -offset; x < s; x += 8) {
                    brush.rect(ctx, x + 1, y + 1, 6, 2, WALL_LIGHT);
                }
            }
        },
    ],
});
