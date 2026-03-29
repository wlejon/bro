cbuffer Viewport : register(b0, space1) {
    float2 viewportSize;
};

struct VSInput {
    float2 pos   : TEXCOORD0;
    float4 color : TEXCOORD1;
};

struct VSOutput {
    float4 color : TEXCOORD0;
    float4 pos   : SV_Position;
};

VSOutput main(VSInput input) {
    VSOutput o;
    float2 ndc = (input.pos / viewportSize) * 2.0 - 1.0;
    ndc.y = -ndc.y;
    o.pos = float4(ndc, 0.0, 1.0);
    o.color = input.color;
    return o;
}
