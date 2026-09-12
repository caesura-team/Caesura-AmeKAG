// Caesura (AmeKAG) - Transition Fragment Shader (Metal)
// crossfade / rule / wipe between two textures
// Corresponds to shaders/dx11/fs_transition.hlsl

#include <metal_stdlib>
using namespace metal;

struct PSInput {
    float4 position [[position]];
    float2 texcoord;
};

struct TransParams {
    float  u_progress;
    float  u_method;
    float2 u_pad;
};

fragment float4 fs_transition(PSInput in [[stage_in]],
                              texture2d<float> s_fromTex [[texture(0)]],
                              texture2d<float> s_toTex   [[texture(1)]],
                              texture2d<float> s_ruleTex [[texture(2)]],
                              sampler s_fromSamp  [[sampler(0)]],
                              sampler s_toSamp    [[sampler(1)]],
                              sampler s_ruleSamp  [[sampler(2)]],
                              constant TransParams& params [[buffer(0)]]) {
    float4 fc = s_fromTex.sample(s_fromSamp, in.texcoord);
    float4 tc = s_toTex.sample(s_toSamp, in.texcoord);
    float t = clamp(params.u_progress, 0.0f, 1.0f);

    switch (int(params.u_method)) {
        case 1: t = step(s_ruleTex.sample(s_ruleSamp, in.texcoord).r, t); break;
        case 2: t = step(in.texcoord.x, t); break;
        case 3: t = step(1.0f - in.texcoord.x, t); break;
        case 4: t = step(1.0f - in.texcoord.y, t); break;
        case 5: t = step(in.texcoord.y, t); break;
    }

    return mix(fc, tc, t);
}
