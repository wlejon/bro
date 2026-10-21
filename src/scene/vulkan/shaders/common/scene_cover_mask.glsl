// The samples of this pixel a cut card keeps: `coverage` of them (the share
// of the pixel its blade, leaf or bloom covers) by a pixel dither salted by
// `salt` (the card's), so two cards over one pixel keep different samples
// and their cover adds up as leaves' would. Under 4x MSAA each sample takes
// its own quarter-step of the dither, so a pixel is kept in quarters and
// resolves to a blend rather than a stipple; without MSAA a pixel is all or
// nothing (the mask is all bits). Fragment stages only.
int coverSampleMask(float coverage, bool msaa, uint salt) {
    // A white-noise value a pixel (an integer hash): a gradient noise lines
    // up into visible diagonal hatching at part covers.
    uvec2 q = uvec2(gl_FragCoord.xy);
    uint v = q.x * 1973u + q.y * 9277u + 0x9e3779b9u;
    v = (v ^ (v >> 16u)) * 0x7feb352du;
    v = (v ^ (v >> 15u)) * 0x846ca68bu;
    v ^= v >> 16u;
    uint v2 = v ^ (salt * 0x9e3779b9u);
    v2 = (v2 ^ (v2 >> 15u)) * 0x2c1b3c6du;
    v2 ^= v2 >> 12u;
    float h = float(v2 >> 16u) * (1.0 / 65536.0);
    if (!msaa) return h < coverage ? -1 : 0;
    int mask = 0;
    for (int s = 0; s < 4; ++s)
        if (fract(h + 0.25 * float(s)) < coverage) mask |= 1 << s;
    return mask;
}
