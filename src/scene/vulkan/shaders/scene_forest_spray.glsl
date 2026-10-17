// A conifer's spray (ForestNode::Look::leafForm 1 or 2): the forest's leaf
// card is the spray's outline, and the needles or the scale-leaf branchlets
// on it are cut out here, as the share of the pixel they cover (mesh.frag
// keeps that share of its samples, forestSampleMask). Analytic, so a spray
// stays sharp up close and averages to its mean cover (ForestNode::sprayFill)
// once its needles are under a pixel, with nothing baked.
//
// The card: `uv` across (0..1) and along (0 at the twig's base, 1 at its
// tip), `aspect` its width over its length. In card lengths, a twig runs up
// the middle and a needle (or a branchlet) leaves it every `spacing` on
// alternate sides at `angle` to it, reaching out to the card's edge in the
// spray's middle and less toward its base and tip.
//
// Needs scene_procedural.glsl (the forest's header).

struct ForestSprayShape {
    float spacing;   // along the twig, between needles on one side
    float angle;     // from the twig, toward the tip (radians)
    float needleHalf;   // a needle's half width
    float scallop;   // a branchlet's scale-leaf lobes: depth (0 a needle)
};

ForestSprayShape forestSprayShape(int form) {
    // Needles: fine and close; scale leaves: broad ropes of overlapping
    // scales, scalloped.
    if (form == 2) return ForestSprayShape(0.08, 0.55, 0.018, 0.35);
    return ForestSprayShape(0.035, 0.9, 0.009, 0.0);
}

float forestSprayHash(float i, float side) {
    return fract(sin(i * 12.9898 + side * 78.233) * 43758.5453);
}

// The spray's reach out from its twig at `v` along it, in card lengths: the
// card's half width through the middle, tapering to the base and tip.
float forestSprayReach(float v, float halfWidth) {
    float t = clamp(v, 0.0, 1.0);
    return halfWidth * sqrt(max(sin(3.14159265 * min(t * 1.08, 1.0)), 0.0)) * smoothstep(0.0, 0.12, t);
}

// The node's spray: its form and its card's width over its length (the
// header's aspect is its needles' cover as an ellipse, ForestNode::gpuData).
int forestSprayForm() { return int(floatBitsToUint(procedural.header[3].w) & 15u); }
float forestSprayCardAspect(int form) {
    float fill = form == 1 ? 0.45 : form == 2 ? 0.47 : 1.0;
    return procedural.header[3].y / (fill * (4.0 / 3.14159265));
}

// The share of the pixel at `uv` the spray covers.
float forestSprayCover(vec2 uv, int form, float aspect) {
    ForestSprayShape sh = forestSprayShape(form);
    float halfWidth = 0.5 * aspect;
    vec2 p = vec2((uv.x - 0.5) * aspect, uv.y);
    float fp = max(max(length(dFdx(p)), length(dFdy(p))), 1e-5);

    // The twig, tapering to its tip.
    float tw = mix(0.012, 0.004, clamp(p.y, 0.0, 1.0));
    float twig = (p.y > 0.0 && p.y < 0.97) ? clamp((tw - abs(p.x)) / fp + 0.5, 0.0, 1.0) : 0.0;

    // The needle family on this side: lines from the twig at `angle`, base
    // `y0` on the twig, one every `spacing` (the sides half a spacing apart).
    float side = p.x >= 0.0 ? 1.0 : -1.0;
    float ax = abs(p.x);
    float sa = sin(sh.angle), ca = cos(sh.angle);
    float t = ax / sa;                    // along the needle
    float y0 = p.y - t * ca;
    float off = side > 0.0 ? 0.0 : 0.5 * sh.spacing;
    float i = floor((y0 - off) / sh.spacing + 0.5);
    float yi = i * sh.spacing + off;
    float d = abs(y0 - yi) * sa;          // across the needle
    float h = forestSprayHash(i, side);
    float len = forestSprayReach(yi, halfWidth) / sa * (0.8 + 0.25 * h);
    float halfW = sh.needleHalf;
    if (sh.scallop > 0.0) halfW *= 1.0 - sh.scallop * abs(sin(t * 3.14159265 / (2.2 * sh.needleHalf)));
    // A needle tapers to its tip.
    halfW *= clamp((len - t) / max(4.0 * sh.needleHalf, 1e-4), 0.25, 1.0);
    bool on = yi > 0.03 && yi < 0.96 && t <= len;
    float sharp = on ? clamp((halfW - d) / fp + 0.5, 0.0, 1.0) * clamp((len - t) / fp + 0.5, 0.0, 1.0) : 0.0;

    // Their mean once a pixel spans a few: the family's share of the band
    // they fill, thinning out to the spray's edge as the needles do (each
    // reaches 0.8 .. 1.05 of the reach, so the share still out there falls
    // across that span): a spray's edge is its needles' ragged tips, not a
    // leaf's clean outline.
    float reach = forestSprayReach(p.y, halfWidth);
    float inside = clamp((1.02 * reach - ax) / max(0.3 * reach + fp, 1e-5), 0.0, 1.0);
    float band = min(2.0 * sh.needleHalf * (1.0 - 0.5 * sh.scallop) / (sh.spacing * sa), 1.0);
    float mean = inside * band;
    // Sharp while two pixels or more part one needle from the next (each is
    // already cut to the share of the pixel it covers), their mean by the
    // time one pixel does.
    float needles = mix(sharp, mean, smoothstep(0.45 * sh.spacing, sh.spacing, fp));
    return max(needles, twig);
}
