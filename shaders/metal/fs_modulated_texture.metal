// Reference Metal source; runtime MSL is generated from the matching .sc file.
#include <metal_stdlib>
using namespace metal;
struct PSInput { float4 position [[position]]; float2 texcoord; };
struct ColorParams { float4 u_color; };
fragment float4 fs_modulated_texture(PSInput in [[stage_in]],
    texture2d<float> s_texture [[texture(0)]], sampler smp [[sampler(0)]],
    constant ColorParams& params [[buffer(0)]]) {
    return s_texture.sample(smp, in.texcoord) * params.u_color;
}
