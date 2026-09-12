// Caesura (AmeKAG) - Fullscreen Quad VS (HLSL vs_4_0)
// Uses standard POSITION+TEXCOORD0 vertex input (not SV_VertexID)
// for bgfx compatibility. Caller passes a fullscreen quad in NDC.
struct VSInput {
    float2 position : POSITION;
    float2 texcoord : TEXCOORD0;
};
struct VSOutput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};
VSOutput main(VSInput input) {
    VSOutput o;
    o.position = float4(input.position, 0.0f, 1.0f);
    o.texcoord = input.texcoord;
    return o;
}
