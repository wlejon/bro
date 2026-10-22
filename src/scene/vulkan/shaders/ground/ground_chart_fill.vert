#version 450
// A turf tile's fill on a round body (ground_layer_fill.frag's
// GROUND_LAYER_CHART build): one triangle over the whole tile and its
// border, each texel's point worked out from where it is in the tile.

void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    // (Reversed-Z: in front of the cleared far plane.)
    gl_Position = vec4(p * 2.0 - 1.0, 0.5, 1.0);
}
