// Caesura (AmeKAG) - 3D LUT color-grade FS (Metal reference, t214)
#include <metal_stdlib>
using namespace metal;
struct PIn { float4 pos [[position]]; float2 uv; };
struct PostFxParams { float4 p0; float4 p1; float4 p2; float4 p3; };
float3 lutCell(texture2d<float> lutTex, sampler smp, float r, float g, float b, float N) {
    return lutTex.sample(smp, float2((b * N + r + 0.5f) / (N * N), (g + 0.5f) / N), level(0.0f)).rgb;
}
float3 lutSlice(texture2d<float> lutTex, sampler smp, float2 rg, float b, float N) {
    float2 lo = floor(rg), hi = min(lo + 1.0f, N - 1.0f), weight = rg - lo;
    return mix(mix(lutCell(lutTex,smp,lo.x,lo.y,b,N),lutCell(lutTex,smp,hi.x,lo.y,b,N),weight.x),
               mix(lutCell(lutTex,smp,lo.x,hi.y,b,N),lutCell(lutTex,smp,hi.x,hi.y,b,N),weight.x),weight.y);
}
fragment float4 fs_postfx_lut3d(PIn in [[stage_in]],
    texture2d<float> tex [[texture(0)]],
    texture2d<float> lutTex [[texture(1)]],
    sampler smp [[sampler(0)]],
    sampler lutSampler [[sampler(1)]],
    constant PostFxParams& u [[buffer(0)]]) {
    float4 src = tex.sample(smp, in.uv);
    float N = max(floor(u.p2.z + 0.5), 2.0);
    float3 coordinate = clamp(src.rgb, 0.0f, 1.0f) * (N - 1.0f);
    float lowB = floor(coordinate.b), highB = min(lowB + 1.0f, N - 1.0f);
    float3 lut = mix(lutSlice(lutTex,lutSampler,coordinate.rg,lowB,N),
                     lutSlice(lutTex,lutSampler,coordinate.rg,highB,N),coordinate.b-lowB);
    float3 outC = mix(src.rgb, lut, clamp(u.p0.x, 0.0f, 1.0f));
    return float4(outC, src.a);
}
