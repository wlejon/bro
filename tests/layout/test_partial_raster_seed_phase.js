// Paint fuzz seed 405252474, step 80: a scroll layer held at a phase a 512th
// of a pixel from the one it was drawn at (the same phase to place it by)
// kept pixels a hair off, 827 of them apart from the whole raster. A partial
// raster now needs the exact phase the layer holds.
require(require('path').join(process.env.BRO_APP_DIR, '..', 'layout', 'partial_raster_seed.js'))(405252474, 81);
