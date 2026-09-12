// Caesura (AmeKAG) - Multi-mode Blend PS (HLSL ps_4_0)
Texture2D    s_baseTex  : register(t0);
SamplerState s_baseSamp : register(s0);
Texture2D    s_blendTex : register(t1);
SamplerState s_blendSamp : register(s1);
cbuffer BlendParams : register(b0) {
    float4 u_opacity;
    float4 u_pad; // CPU uploads two vec4s; mode is u_opacity.w, not offset 16.
};
struct PSInput { float4 p : SV_POSITION; float2 t : TEXCOORD0; };

float3 bAdd(float3 a, float3 b) { return a+b; }
float3 bSub(float3 a, float3 b) { return a-b; }
float3 bMul(float3 a, float3 b) { return a*b; }
float3 bScr(float3 a, float3 b) { return 1-(1-a)*(1-b); }
float3 bOvl(float3 a, float3 b) {
    float3 r;
    r.r = a.r<0.5 ? 2*a.r*b.r : 1-2*(1-a.r)*(1-b.r);
    r.g = a.g<0.5 ? 2*a.g*b.g : 1-2*(1-a.g)*(1-b.g);
    r.b = a.b<0.5 ? 2*a.b*b.b : 1-2*(1-a.b)*(1-b.b);
    return r;
}
float3 bDrk(float3 a, float3 b) { return min(a,b); }
float3 bLit(float3 a, float3 b) { return max(a,b); }
float3 bDif(float3 a, float3 b) { return abs(a-b); }

float4 main(PSInput i) : SV_TARGET {
    float4 base  = s_baseTex.Sample(s_baseSamp, i.t) * u_opacity.x;
    float4 blend = s_blendTex.Sample(s_blendSamp, i.t) * u_opacity.y;
    float3 c;
    switch(int(u_opacity.w)) {
        case 1: c=bAdd(base.rgb,blend.rgb); break;
        case 2: c=bSub(base.rgb,blend.rgb); break;
        case 3: c=bMul(base.rgb,blend.rgb); break;
        case 4: c=bScr(base.rgb,blend.rgb); break;
        case 5: c=bOvl(base.rgb,blend.rgb); break;
        case 6: c=bDrk(base.rgb,blend.rgb); break;
        case 7: c=bLit(base.rgb,blend.rgb); break;
        case 8: c=bDif(base.rgb,blend.rgb); break;
        default: c=lerp(base.rgb,blend.rgb,blend.a); break;
    }
    return float4(c, saturate(base.a+blend.a)) * u_opacity.z;
}
