#version 450

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec2 inTexUV;

layout(set = 1, binding = 0) uniform sampler2D uTex;

layout(push_constant) uniform BillboardPush {
    vec4 anchor;     // xyz = worldAnchor, w = unused
    vec4 right;      // xyz = billboard right, w = unused
    vec4 up;         // xyz = billboard up, w = unused
    vec2 halfSize;
    vec2 uvMin;
    vec2 uvMax;
    float strokeWidth;
    int shapeMode;
    vec4 color;
    vec4 stroke;
} push;

layout(location = 0) out vec4 fragColor;

void main() {
    if (push.shapeMode == 0) {
        // Rect: solid fill with optional inset stroke.
        vec4 c = push.color;
        if (push.strokeWidth > 0.0) {
            vec2 d = min(inUV, 1.0 - inUV);
            float border = min(d.x, d.y);
            if (border < push.strokeWidth) c = push.stroke;
        }
        if (c.a <= 0.0) discard;
        // Straight-alpha input — premultiply for "over" blend.
        fragColor = vec4(c.rgb * c.a, c.a);
    } else if (push.shapeMode == 1) {
        // Circle SDF centered on UV (0.5, 0.5), radius 0.5.
        vec2 p = inUV - 0.5;
        float d = length(p) * 2.0;
        float aa = fwidth(d);
        float alpha = 1.0 - smoothstep(1.0 - aa, 1.0, d);
        if (alpha <= 0.0) discard;
        float a = push.color.a * alpha;
        fragColor = vec4(push.color.rgb * a, a);
    } else if (push.shapeMode == 3) {
        // Filled disc with a ring border.
        vec2 p = inUV - 0.5;
        float d = length(p) * 2.0;
        float aa = fwidth(d);
        float alpha = 1.0 - smoothstep(1.0 - aa, 1.0, d);
        if (alpha <= 0.0) discard;
        float inner = 1.0 - clamp(push.strokeWidth, 0.0, 1.0);
        float ringT = smoothstep(inner - aa, inner + aa, d);
        vec4 c = mix(push.color, push.stroke, ringT);
        float a = c.a * alpha;
        fragColor = vec4(c.rgb * a, a);
    } else if (push.shapeMode == 4) {
        // Textured with straight-alpha source (sprite RGBA from broimage).
        vec4 tex = texture(uTex, inTexUV);
        float a = tex.a * push.color.a;
        if (a <= 0.0) discard;
        fragColor = vec4(tex.rgb * push.color.rgb * a, a);
    } else {
        // Textured (premultiplied alpha from Skia surfaces — HtmlNode).
        vec4 tex = texture(uTex, inTexUV);
        vec4 c = tex * push.color;
        if (c.a <= 0.0) discard;
        fragColor = c;
    }
}
