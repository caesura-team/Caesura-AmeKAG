// Caesura (AmeKAG) - Transition PS (HLSL ps_4_0)
Texture2D    s_fromTex  : register(t0);
SamplerState s_fromSamp : register(s0);
Texture2D    s_toTex    : register(t1);
SamplerState s_toSamp   : register(s1);
Texture2D    s_ruleTex  : register(t2);
SamplerState s_ruleSamp : register(s2);
cbuffer TransParams : register(b0) { float u_progress; float u_method; float2 u_pad; };
struct PSInput { float4 p : SV_POSITION; float2 t : TEXCOORD0; };

float4 main(PSInput i) : SV_TARGET {
    float4 fc = s_fromTex.Sample(s_fromSamp, i.t);
    float4 tc = s_toTex.Sample(s_toSamp, i.t);
    float t = saturate(u_progress);
    switch(int(u_method)) {
        case 1: t = step(s_ruleTex.Sample(s_ruleSamp, i.t).r, t); break;
        case 2: t = step(i.t.x, t); break;
        case 3: t = step(1-i.t.x, t); break;
        case 4: t = step(1-i.t.y, t); break;
        case 5: t = step(i.t.y, t); break;
    }
    return lerp(fc, tc, t);
}
