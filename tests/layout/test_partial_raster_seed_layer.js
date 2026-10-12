// Paint fuzz seed 213315603 (found on Windows), step 66: text and rounded
// rects in an opacity layer, redrawn under a clip round the changed rect,
// came out a level or two apart from the whole raster in 5 pixels. A layer's
// device is the clip it opens under, and what it holds rasters against it;
// a layer reaching the changed rect now rasters the scroll layer whole.
require(require('path').join(process.env.BRO_APP_DIR, '..', 'layout', 'partial_raster_seed.js'))(213315603, 67);
