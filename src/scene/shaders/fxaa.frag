// FXAA 3.11, quality preset — runs LAST in the post stack, on the final LDR
// image (after tonemap / LUT / tilt-shift). Structure follows the reference
// quality path: local-contrast early exit, horizontal/vertical edge classify,
// 12-step edge-end search with progressive stride, edge-center offset, plus
// the sub-pixel aliasing filter. Complements MSAA rather than replacing it:
// MSAA resolves geometry edges in HDR, FXAA additionally smooths shader /
// specular / post-pass aliasing on the LDR result — both can be on.
//
// All four channels are sampled/blended together so scene alpha follows the
// anti-aliased edges for the compositor.

#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform vec2 uTexelSize;   // 1 / resolution
out vec4 FragColor;

#define EDGE_THRESHOLD_MIN 0.0312
#define EDGE_THRESHOLD_MAX 0.125
#define ITERATIONS 12
#define SUBPIXEL_QUALITY 0.75

float lumaOf(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

float stepQuality(int i) {
    // 3.11 quality-12 stride table: 1,1,1,1,1,1.5,2,2,2,2,4,8.
    if (i < 5) return 1.0;
    if (i == 5) return 1.5;
    if (i < 10) return 2.0;
    if (i == 10) return 4.0;
    return 8.0;
}

void main() {
    vec4 center = texture(uTex, vUV);
    float lumaCenter = lumaOf(center.rgb);
    float lumaDown  = lumaOf(textureOffset(uTex, vUV, ivec2( 0, -1)).rgb);
    float lumaUp    = lumaOf(textureOffset(uTex, vUV, ivec2( 0,  1)).rgb);
    float lumaLeft  = lumaOf(textureOffset(uTex, vUV, ivec2(-1,  0)).rgb);
    float lumaRight = lumaOf(textureOffset(uTex, vUV, ivec2( 1,  0)).rgb);

    float lumaMin = min(lumaCenter, min(min(lumaDown, lumaUp), min(lumaLeft, lumaRight)));
    float lumaMax = max(lumaCenter, max(max(lumaDown, lumaUp), max(lumaLeft, lumaRight)));
    float lumaRange = lumaMax - lumaMin;

    // Early exit: not on a visible edge.
    if (lumaRange < max(EDGE_THRESHOLD_MIN, lumaMax * EDGE_THRESHOLD_MAX)) {
        FragColor = center;
        return;
    }

    float lumaDownLeft  = lumaOf(textureOffset(uTex, vUV, ivec2(-1, -1)).rgb);
    float lumaUpRight   = lumaOf(textureOffset(uTex, vUV, ivec2( 1,  1)).rgb);
    float lumaUpLeft    = lumaOf(textureOffset(uTex, vUV, ivec2(-1,  1)).rgb);
    float lumaDownRight = lumaOf(textureOffset(uTex, vUV, ivec2( 1, -1)).rgb);

    float lumaDownUp    = lumaDown + lumaUp;
    float lumaLeftRight = lumaLeft + lumaRight;
    float lumaLeftCorners  = lumaDownLeft + lumaUpLeft;
    float lumaDownCorners  = lumaDownLeft + lumaDownRight;
    float lumaRightCorners = lumaDownRight + lumaUpRight;
    float lumaUpCorners    = lumaUpRight + lumaUpLeft;

    float edgeHorizontal = abs(-2.0 * lumaLeft + lumaLeftCorners)
                         + abs(-2.0 * lumaCenter + lumaDownUp) * 2.0
                         + abs(-2.0 * lumaRight + lumaRightCorners);
    float edgeVertical   = abs(-2.0 * lumaUp + lumaUpCorners)
                         + abs(-2.0 * lumaCenter + lumaLeftRight) * 2.0
                         + abs(-2.0 * lumaDown + lumaDownCorners);
    bool isHorizontal = edgeHorizontal >= edgeVertical;

    // Gradient direction perpendicular to the edge.
    float luma1 = isHorizontal ? lumaDown : lumaLeft;
    float luma2 = isHorizontal ? lumaUp : lumaRight;
    float gradient1 = luma1 - lumaCenter;
    float gradient2 = luma2 - lumaCenter;
    bool is1Steepest = abs(gradient1) >= abs(gradient2);
    float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

    float stepLength = isHorizontal ? uTexelSize.y : uTexelSize.x;
    float lumaLocalAverage;
    if (is1Steepest) {
        stepLength = -stepLength;
        lumaLocalAverage = 0.5 * (luma1 + lumaCenter);
    } else {
        lumaLocalAverage = 0.5 * (luma2 + lumaCenter);
    }

    // Shift half a pixel to the edge line.
    vec2 currentUv = vUV;
    if (isHorizontal) currentUv.y += stepLength * 0.5;
    else              currentUv.x += stepLength * 0.5;

    // Explore along the edge in both directions.
    vec2 offset = isHorizontal ? vec2(uTexelSize.x, 0.0)
                               : vec2(0.0, uTexelSize.y);
    vec2 uv1 = currentUv - offset;
    vec2 uv2 = currentUv + offset;

    float lumaEnd1 = lumaOf(texture(uTex, uv1).rgb) - lumaLocalAverage;
    float lumaEnd2 = lumaOf(texture(uTex, uv2).rgb) - lumaLocalAverage;
    bool reached1 = abs(lumaEnd1) >= gradientScaled;
    bool reached2 = abs(lumaEnd2) >= gradientScaled;
    bool reachedBoth = reached1 && reached2;
    if (!reached1) uv1 -= offset;
    if (!reached2) uv2 += offset;

    if (!reachedBoth) {
        for (int i = 2; i < ITERATIONS; i++) {
            if (!reached1) {
                lumaEnd1 = lumaOf(texture(uTex, uv1).rgb) - lumaLocalAverage;
                reached1 = abs(lumaEnd1) >= gradientScaled;
            }
            if (!reached2) {
                lumaEnd2 = lumaOf(texture(uTex, uv2).rgb) - lumaLocalAverage;
                reached2 = abs(lumaEnd2) >= gradientScaled;
            }
            reachedBoth = reached1 && reached2;
            if (!reached1) uv1 -= offset * stepQuality(i);
            if (!reached2) uv2 += offset * stepQuality(i);
            if (reachedBoth) break;
        }
    }

    float distance1 = isHorizontal ? (vUV.x - uv1.x) : (vUV.y - uv1.y);
    float distance2 = isHorizontal ? (uv2.x - vUV.x) : (uv2.y - vUV.y);
    bool isDirection1 = distance1 < distance2;
    float distanceFinal = min(distance1, distance2);
    float edgeThickness = distance1 + distance2;

    // Only shift toward an edge end whose luma variation confirms we're on
    // the darker/lighter side consistently.
    bool isLumaCenterSmaller = lumaCenter < lumaLocalAverage;
    bool correctVariation1 = (lumaEnd1 < 0.0) != isLumaCenterSmaller;
    bool correctVariation2 = (lumaEnd2 < 0.0) != isLumaCenterSmaller;
    bool correctVariation = isDirection1 ? correctVariation1 : correctVariation2;

    float pixelOffset = -distanceFinal / edgeThickness + 0.5;
    float finalOffset = correctVariation ? pixelOffset : 0.0;

    // Sub-pixel aliasing filter.
    float lumaAverage = (1.0 / 12.0) * (2.0 * (lumaDownUp + lumaLeftRight)
                        + lumaLeftCorners + lumaRightCorners);
    float subPixelOffset1 = clamp(abs(lumaAverage - lumaCenter) / lumaRange, 0.0, 1.0);
    float subPixelOffset2 = (-2.0 * subPixelOffset1 + 3.0) * subPixelOffset1 * subPixelOffset1;
    float subPixelOffsetFinal = subPixelOffset2 * subPixelOffset2 * SUBPIXEL_QUALITY;
    finalOffset = max(finalOffset, subPixelOffsetFinal);

    vec2 finalUv = vUV;
    if (isHorizontal) finalUv.y += finalOffset * stepLength;
    else              finalUv.x += finalOffset * stepLength;

    FragColor = texture(uTex, finalUv);
}
