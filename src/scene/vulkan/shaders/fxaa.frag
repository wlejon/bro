#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D texSource;

layout(push_constant) uniform FxaaPushConstants {
    vec2 texelSize;
} push;

float rgb2luma(vec3 rgb) {
    return dot(rgb, vec3(0.299, 0.587, 0.114));
}

void main() {
    vec4 srcM = texture(texSource, inUV);
    vec3 rgbM = srcM.rgb;
    vec3 rgbNW = texture(texSource, inUV + vec2(-push.texelSize.x, -push.texelSize.y)).rgb;
    vec3 rgbNE = texture(texSource, inUV + vec2( push.texelSize.x, -push.texelSize.y)).rgb;
    vec3 rgbSW = texture(texSource, inUV + vec2(-push.texelSize.x,  push.texelSize.y)).rgb;
    vec3 rgbSE = texture(texSource, inUV + vec2( push.texelSize.x,  push.texelSize.y)).rgb;

    float lumaM  = rgb2luma(rgbM);
    float lumaNW = rgb2luma(rgbNW);
    float lumaNE = rgb2luma(rgbNE);
    float lumaSW = rgb2luma(rgbSW);
    float lumaSE = rgb2luma(rgbSE);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    if (lumaMax - lumaMin < max(0.05, lumaMax * 0.125)) {
        outColor = vec4(rgbM, srcM.a);
        return;
    }

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * (1.0 / 8.0)), 1.0 / 128.0);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = min(vec2(8.0), max(vec2(-8.0), dir * rcpDirMin)) * push.texelSize;

    vec3 rgbA = 0.5 * (
        texture(texSource, inUV + dir * (1.0 / 3.0 - 0.5)).rgb +
        texture(texSource, inUV + dir * (2.0 / 3.0 - 0.5)).rgb
    );
    vec3 rgbB = rgbA * 0.5 + 0.25 * (
        texture(texSource, inUV + dir * -0.5).rgb +
        texture(texSource, inUV + dir *  0.5).rgb
    );

    float lumaB = rgb2luma(rgbB);
    if (lumaB < lumaMin || lumaB > lumaMax) {
        outColor = vec4(rgbA, srcM.a);
    } else {
        outColor = vec4(rgbB, srcM.a);
    }
}
