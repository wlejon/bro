# Art Station

A code-driven sprite/tileset authoring tool, designed for headless workflows.
You describe assets as JS draw functions; the app rasterizes them on a 2D
canvas and exports pixel-perfect, transparent-background PNGs ready to feed
back into `scene.createSprite` / `scene.createTilemap`.

There is no painting GUI — every pixel is described in code. This is the
point: code is reproducible, diffable, and easy to iterate on without an
input device.

## Workflow

1. Author an asset under `assets/<name>.js`. The module calls `defineSheet`
   or `defineTileset` with frame draw functions.
2. From a headless script, `load('<name>')` then `render()` then `save('<name>')`.
3. The PNG lands at `output/<name>.png` (transparent bg) plus a sidecar
   `output/<name>.json` with the manifest (frame size, animations, ...).
4. Use the PNG directly with `scene.createSprite({ src: '...png', sheet: ... })`.

```bash
# One-off render + save
bro-headless apps/artstation -e "load('blob'); render(); flush(); save('blob')"

# Or via a script with multiple assets
bro-headless apps/artstation render_all.js
```

## Defining a sprite sheet

```js
defineSheet('hero', {
    frameWidth: 32, frameHeight: 32,
    cols: 8, rows: 4,           // grid layout in the sheet
    bg: 'transparent',
    frames: [
        (ctx, w, h, i) => { /* draw frame 0 */ },
        // ... null entries skip a cell
    ],
    animations: {
        idle: { frames: [0,1,2,3], fps: 4,  loop: true },
        walk: { frames: [8,9,10,11,12,13,14,15], fps: 12, loop: true, next: 'idle' },
    },
});
```

Frame functions are invoked with the canvas already translated to the
frame's top-left and clipped to the frame rect — so coordinates are local
to the frame. `imageSmoothingEnabled` is off; integer coords give crisp
pixel art.

## Defining a procedural animation

For animations driven by *state* (particles, physics, IK, springs, anything
stepping over time) instead of N hand-drawn cells, use `defineAnimated`.
The framework steps headless virtual time, calls your `frame` function once
per tick, and tiles the captures into a regular spritesheet — so playback
is identical to a hand-laid `defineSheet`.

```js
defineAnimated('explosion', {
    frameWidth: 32, frameHeight: 32,
    fps: 24, duration: 0.75,        // → 18 frames
    cols: 6,                         // sheet layout (rows auto)
    bg: 'transparent', pixel: true,

    init() {
        // Returns the state object passed into every frame() call.
        return { particles: seedParticles() };
    },

    frame(ctx, w, h, t, dt, state) {
        // ctx is pre-translated/clipped to the current cell.
        // t = elapsed seconds, dt = 1/fps (constant).
        for (const p of state.particles) {
            p.x += p.vx * dt; p.y += p.vy * dt;
            brush.px(ctx, p.x, p.y, p.color);
        }
    },

    // Optional. Defaults to one 'play' animation covering all frames.
    animations: { play: { frames: 'all', fps: 24, loop: false } },
});
```

The PNG that lands in `output/` is shaped like any other sprite sheet, so
`scene.createSprite({ src, sheet, animations })` plays it back without
caring that the source was procedural. Use a deterministic PRNG in `init`
if you want byte-stable renders across runs.

Animated assets can also be encoded to a WebM/VP9 video or animated GIF via
`saveVideo()` / `saveGif()`. Both walk the same virtual-time loop and write
one frame per cell:

```bash
bro-headless apps/artstation -e "load('explosion'); saveVideo(); saveGif()"
# → apps/artstation/output/explosion.webm
# → apps/artstation/output/explosion.gif
```

Frame width/height must be even (VP9 4:2:0 chroma). The asset's `fps` and
`duration` drive video frame rate and length. Override per-call:
`saveVideo(name, { quality: 'best', bitrateKbps: 2000 })` or
`saveGif(name, { paletteBits: 6, loopCount: 0 })`.

`defineSheet` assets work too — saveVideo/saveGif walk the chosen
animation's frame indices and produce a video looping that animation. Pick
the animation with `opts.anim`, otherwise the first one wins (preferring
`idle` / `play` / `loop`). Output filename gets the animation suffix:

```bash
bro-headless apps/artstation -e "load('blob'); saveGif('blob', { anim: 'walk' })"
# → apps/artstation/output/blob_walk.gif
```

In windowed mode, three buttons in the save bar above the canvases call
`save()` / `saveVideo()` / `saveGif()` for the currently-selected asset.
PNG is enabled for any asset; WebM/GIF only for animated and sheet kinds.

## Defining a tileset

```js
defineTileset('terrain', {
    tileSize: 16,
    cols: 8,
    bg: 'transparent',
    tiles: [
        null,                              // index 0 reserved (engine treats 0 as empty)
        (ctx, s, i) => { /* tile 1: grass */ },
        (ctx, s, i) => { /* tile 2: dirt */ },
        // ...
    ],
});
```

## Drawing helpers (`brush.*`)

`brush.js` ships small primitives tuned for pixel art:

| Helper | What it does |
|--------|--------------|
| `brush.px(ctx, x, y, color)` | Single pixel |
| `brush.hline / vline / line` | Pixel-perfect lines |
| `brush.rect / rectOutline` | Filled / outlined rect |
| `brush.circle / circleOutline` | Filled / outlined circle |
| `brush.stamp(ctx, x, y, rows, palette)` | Paint from an ASCII grid |
| `brush.gradV(ctx, x, y, w, h, top, bot, steps)` | Quantized vertical gradient |
| `brush.mirrorH(ctx, x, y, w, h)` | Mirror left half to right (for symmetric art) |
| `brush.PICO8`, `brush.ENDESGA16` | Hand-tuned 16-color palettes |

## Headless globals exposed by the app

| Global | What it does |
|--------|--------------|
| `load(name)` | Load `assets/<name>.js` and register its asset(s) |
| `render(name?)` | Render the current (or named) asset to the sheet canvas |
| `save(name?)` | Write the sheet PNG (alpha preserved) + sidecar manifest JSON |
| `inspectArt(scale?)` | Re-render at integer scale into the inspect canvas (debug) |
| `saveVideo(name?, opts?)` | Encode `defineAnimated` or `defineSheet` to `output/<name>.webm` (VP9). Opts: path, fps, bitrateKbps, quality, anim |
| `saveGif(name?, opts?)` | Encode `defineAnimated` or `defineSheet` to `output/<name>.gif` (GIF89a). Opts: path, fps, paletteBits, loopCount, anim |
| `preview(animName?)` | Animate the saved sprite back through the scene API on the stage canvas |
| `previewMap(layoutFn?)` | Lay the tileset into a small tilemap on the stage canvas |
| `listAssets()` | Names + kinds of every loaded asset |

## Verifying output

`screenshotCanvas(path, '#sheet')` writes the canvas's Skia surface
straight to PNG with full alpha — the regular `screenshot()` flattens
transparent pixels to opaque black during framebuffer composite.

For visual inspection, upscale the PNG and view it as a normal image:

```bash
# 8x nearest-neighbor upscale via Pillow
python -c "from PIL import Image; im=Image.open('output/blob.png'); \
  im.resize((im.size[0]*8, im.size[1]*8), Image.NEAREST).save('output/_blob_zoom.png')"
```

## Why no painting UI?

The user is an AI model. It can write JS that places a pixel exactly where
intended; it cannot wield a brush in real time. Code-as-art means every
asset has a reproducible recipe — change `BODY = '#e53b44'` → `'#3b82f6'`
and the same blob comes back blue, instantly, without re-painting.
