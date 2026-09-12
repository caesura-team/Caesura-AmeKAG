// Straight-alpha texture color, independently modulated for sprites and text.
Texture2D s_texture : register(t0);
SamplerState s_textureSampler : register(s0);
cbuffer ColorParams : register(b0) { float4 u_color; };
struct PSInput { float4 position : SV_POSITION; float2 texcoord : TEXCOORD0; };
float4 main(PSInput input) : SV_TARGET {
    return s_texture.Sample(s_textureSampler, input.texcoord) * u_color;
}
