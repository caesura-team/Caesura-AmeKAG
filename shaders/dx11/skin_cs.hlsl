// Caesura (AmeKAG) - SMA S5 GPU skinning compute shader (cs_5_0)
// Reads per-vertex {pos(2), uv(2), bone0/bone1(2), w0/w1(2)} (input VB
// layout stride 32B). Snapshot row0=(x,y,scale,reserved), row1=(viewW,
// viewH,uintBits(vertexCount),uintBits(poseCount)), rows2+i=actual pose i
// (cos*scale,sin*scale,ox,oy). Writes the FINAL NDC
// positions + UV (output VB stride 16B). Registers match the binding
// stages: input t0 (stage 0), bones t1 (stage 1), output u2 (stage 2).
Buffer<float4> skinInput : register(t0);
Buffer<float4> skinBones : register(t1);
RWBuffer<float4> skinOutput : register(u2);

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    uint vid = dtid.x;
    float4 metadata = skinBones[1];
    if (vid >= asuint(metadata.z)) return;
    float4 a = skinInput[vid * 2u];       // pos.xy, uv.xy
    float4 b = skinInput[vid * 2u + 1u];  // bone0, bone1, w0, w1
    float2 pos = a.xy;
    float2 uv = a.zw;
    float2 bones = b.xy;
    float2 weights = b.zw;

    uint poseCount = asuint(metadata.w);
    uint b0 = (uint)bones.x, b1 = (uint)bones.y;
    float2 skinned = pos;
    // Match skinMesh exactly: invalid p0 stays in place, even if p1 exists.
    if (b0 < poseCount) {
        float4 m0 = skinBones[2u + b0];
        float2 p0 = float2(m0.x * pos.x - m0.y * pos.y + m0.z,
                           m0.y * pos.x + m0.x * pos.y + m0.w);
        if (b1 < poseCount) {
            float wsum = weights.x + weights.y;
            if (wsum <= 0.0) {
                skinned = pos;
            } else {
                float4 m1 = skinBones[2u + b1];
                float2 p1 = float2(m1.x * pos.x - m1.y * pos.y + m1.z,
                                   m1.y * pos.x + m1.x * pos.y + m1.w);
                skinned = (p0 * weights.x + p1 * weights.y) / wsum;
            }
        } else {
            // A lone valid p0 applies regardless of either weight.
            skinned = p0;
        }
    }
    float4 drawP = skinBones[0];
    float4 viewP = metadata;
    float2 px = float2(drawP.x + skinned.x * drawP.z,
                       drawP.y + skinned.y * drawP.z);
    float2 ndc = float2(px.x / viewP.x * 2.0 - 1.0,
                        1.0 - px.y / viewP.y * 2.0);
    skinOutput[vid] = float4(ndc, uv);
}
