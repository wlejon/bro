
#version 330 core
in vec2 vUV;
in vec2 vTexUV;

uniform int uShapeMode;      // 0 = rect, 1 = circle SDF, 2 = textured, 3 = ringed disc
uniform vec4 uColor;         // rect / circle fill, or tint for texture
uniform vec4 uStroke;
uniform float uStrokeWidth;  // in UV units (0 = no stroke)
uniform sampler2D uTex;

out vec4 FragColor;

void main() {
    if (uShapeMode == 0) {
        // Rect: solid fill with optional inset stroke.
        vec4 c = uColor;
        if (uStrokeWidth > 0.0) {
            vec2 d = min(vUV, 1.0 - vUV);
            float border = min(d.x, d.y);
            if (border < uStrokeWidth) c = uStroke;
        }
        if (c.a <= 0.0) discard;
        // Straight-alpha input — premultiply for "over" blend.
        FragColor = vec4(c.rgb * c.a, c.a);
    } else if (uShapeMode == 1) {
        // Circle SDF centered on UV (0.5, 0.5), radius 0.5.
        vec2 p = vUV - 0.5;
        float d = length(p) * 2.0;
        float aa = fwidth(d);
        float alpha = 1.0 - smoothstep(1.0 - aa, 1.0, d);
        if (alpha <= 0.0) discard;
        float a = uColor.a * alpha;
        FragColor = vec4(uColor.rgb * a, a);
    } else if (uShapeMode == 3) {
        // Filled disc with a ring border. uStrokeWidth is ring thickness as
        // a fraction of the radius (0.2 = outer 20% is ring). Used for
        // engine-drawn gizmo/editor icons (e.g. light markers).
        vec2 p = vUV - 0.5;
        float d = length(p) * 2.0;
        float aa = fwidth(d);
        float alpha = 1.0 - smoothstep(1.0 - aa, 1.0, d);
        if (alpha <= 0.0) discard;
        float inner = 1.0 - clamp(uStrokeWidth, 0.0, 1.0);
        float ringT = smoothstep(inner - aa, inner + aa, d);
        vec4 c = mix(uColor, uStroke, ringT);
        float a = c.a * alpha;
        FragColor = vec4(c.rgb * a, a);
    } else if (uShapeMode == 4) {
        // Textured with straight-alpha source (sprite RGBA from broimage).
        vec4 tex = texture(uTex, vTexUV);
        float a = tex.a * uColor.a;
        if (a <= 0.0) discard;
        FragColor = vec4(tex.rgb * uColor.rgb * a, a);
    } else {
        // Textured (premultiplied alpha from Skia surfaces — HtmlNode).
        vec4 tex = texture(uTex, vTexUV);
        vec4 c = tex * uColor; // uColor.a tints premultiplied texture
        if (c.a <= 0.0) discard;
        FragColor = c;
    }
}
