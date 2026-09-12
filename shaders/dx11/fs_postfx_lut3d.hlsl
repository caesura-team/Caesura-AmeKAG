// Caesura (AmeKAG) - 3D LUT color-grade PS (HLSL ps_4_0, t214)
// 2D-packed cube: x=b*N+r, y=g. Source RGB selects the cube entry.
// Explicit R/G bilinear and B-slice interpolation does not depend on sampler
// filtering flags. Every read uses a texel center at LOD0.
Texture2D    s_tex  : register(t0);
Texture2D    s_lut  : register(t1);
SamplerState s_samp : register(s0);
SamplerState s_lutSamp : register(s1);
cbuffer PostFxParams : register(b0) {
    float4 u_p0; // x=intensity(0..1)
    float4 u_p1; // unused
    float4 u_p2; // (1/w, 1/h, lutSize N, 0)
    float4 u_p3; // spare
};
struct PSInput { float4 p : SV_POSITION; float2 t : TEXCOORD0; };

float3 lutCell(float r, float g, float b, float N) {
    float2 uv = float2((b * N + r + 0.5) / (N * N), (g + 0.5) / N);
    return s_lut.SampleLevel(s_lutSamp, uv, 0.0).rgb;
}
float3 lutSlice(float2 rg, float b, float N) {
    float2 lo = floor(rg);
    float2 hi = min(lo + 1.0, N - 1.0);
    float2 weight = rg - lo;
    return lerp(lerp(lutCell(lo.x, lo.y, b, N), lutCell(hi.x, lo.y, b, N), weight.x),
                lerp(lutCell(lo.x, hi.y, b, N), lutCell(hi.x, hi.y, b, N), weight.x), weight.y);
}
float4 main(PSInput i) : SV_TARGET {
    float4 src = s_tex.Sample(s_samp, i.t);
    float  N    = max(floor(u_p2.z + 0.5), 2.0);
    float3 coordinate = saturate(src.rgb) * (N - 1.0);
    float lowB = floor(coordinate.b);
    float highB = min(lowB + 1.0, N - 1.0);
    float3 lut = lerp(lutSlice(coordinate.rg, lowB, N), lutSlice(coordinate.rg, highB, N), coordinate.b - lowB);
    float3 outC = lerp(src.rgb, lut, saturate(u_p0.x));
    return float4(outC, src.a);
}
