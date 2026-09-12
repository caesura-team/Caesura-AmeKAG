// Caesura (AmeKAG) - Fullscreen Quad Vertex Shader (Metal)
// Standard NDC POSITION/TEXCOORD quad, matching the production draw buffers.
// Corresponds to shaders/dx11/vs_fullscreen.hlsl

#include <metal_stdlib>
using namespace metal;

struct VSOutput {
    float4 position [[position]];
    float2 texcoord;
};
struct VSInput {
    float2 position [[attribute(0)]];
    float2 texcoord [[attribute(1)]];
};

vertex VSOutput vs_fullscreen(VSInput in [[stage_in]]) {
    VSOutput out;
    out.position = float4(in.position, 0.0, 1.0);
    out.texcoord = in.texcoord;
    return out;
}
