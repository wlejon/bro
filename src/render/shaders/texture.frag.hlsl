Texture2D<float4> Tex : register(t0, space2);
SamplerState Samp : register(s0, space2);

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return Tex.Sample(Samp, uv);
}
