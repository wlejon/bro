// Far cards (docs/scene-world-architecture.md §2, scene_far_cards.h): the
// layout C++ writes and far_card.vert / far_card.frag read. Shared by both,
// so plain #defines only.
//
// A card is a far cell's crowns as the camera last saw them: a tile of the
// card atlas holding their resolved colour and the share of the pixel's
// samples they covered, and their nearest depth, rendered through the
// camera's own projection cropped to the cell's rectangle on screen. A record
// says how to take a tile point back to where it lay, relative to the eye
// now, and which share of each pixel's samples the card keeps (the hand-over
// from the real crowns, ForestNode::fadeCode's window).
//
// The atlas is one RG32UI image, 8 bytes a texel: x the covered samples'
// mean colour as RGB9E5 (not premultiplied); y the nearest depth in 24 bits
// across the card's own depth range (record vec4 6) under the count of
// covered samples in the top 8 (0: the texel holds nothing).

#ifndef FAR_CARD_LAYOUT_H
#define FAR_CARD_LAYOUT_H

// vec4s a card record:
//   0-3  the inverse of the capture's crop view-projection, its result taken
//        relative to the eye now (columns): (tile ndc, depth) -> position;
//   4    the tile in the atlas: u0, v0, u1, v1 (normalised);
//   5    x: the share of samples from which the card draws (its window
//        [x, 1), the real crowns' [0, x)); y: the depth to stand empty grid
//        points at (the cell's centre seen from the capture, in the capture's
//        depth); z: the dither salt; w: the tile's texels a side;
//   6    x, y: the capture depths the 24-bit depth spans; z: the samples a
//        pixel at capture; w: unused.
#define FAR_CARD_RECORD_VEC4S 7

// A card's grid: a quad each FAR_CARD_GRID_TEXELS texels a side (at least
// one), each grid point at the nearest depth the card holds round it, so the
// card bends over the crowns' relief and keeps their parallax as the eye
// moves on from where it was captured. Cards draw grouped by tile size, a
// draw a grid.
#define FAR_CARD_GRID_TEXELS 16

#define FAR_CARD_DEPTH_BITS 24
#define FAR_CARD_DEPTH_MAX 16777215u

// The cells image (R32UI, the atlas's size over FAR_CARD_GRID_TEXELS): a
// texel a grid cell of a tile, its covered texels' nearest depth, so a grid
// point stood at its cells' nearest lies in front of every texel round it
// (and the fragment's depth, clamped to the point's, gives early depth
// tests). 0 for a cell with nothing covered, else the nearest's 24 bits
// plus one, counted toward the eye: q + 1 under REVERSED_Z (the nearest
// the largest), FAR_CARD_DEPTH_MAX - q + 1 otherwise, so the nearest of
// several is always the largest.

#endif
