#version 430 core
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(std430, binding = 0) buffer SkinInput  { vec4 inData[]; };
layout(std430, binding = 1) buffer SkinBones  { vec4 boneData[]; };
layout(std430, binding = 2) buffer SkinOutput { vec4 outData[]; };

void main()
{
    uint vid = gl_GlobalInvocationID.x;
    // row0 draw, row1 view/count metadata, rows2+i actual pose i.
    vec4 metadata = boneData[1];
    if (vid >= floatBitsToUint(metadata.z)) return;
    vec4 a = inData[vid * 2u];       // pos.xy, uv.xy
    vec4 b = inData[vid * 2u + 1u];  // bone0, bone1, w0, w1
    vec2 pos = a.xy;
    vec2 uv = a.zw;
    vec2 bones = b.xy;
    vec2 weights = b.zw;

    uint poseCount = floatBitsToUint(metadata.w);
    uint b0 = uint(bones.x), b1 = uint(bones.y);
    vec2 skinned = pos;
    if (b0 < poseCount)
    {
        vec4 m0 = boneData[2u + b0];
        vec2 p0 = vec2(m0.x * pos.x - m0.y * pos.y + m0.z,
                       m0.y * pos.x + m0.x * pos.y + m0.w);
        if (b1 < poseCount)
        {
            float wsum = weights.x + weights.y;
            if (wsum <= 0.0) skinned = pos;
            else
            {
                vec4 m1 = boneData[2u + b1];
                vec2 p1 = vec2(m1.x * pos.x - m1.y * pos.y + m1.z,
                               m1.y * pos.x + m1.x * pos.y + m1.w);
                skinned = (p0 * weights.x + p1 * weights.y) / wsum;
            }
        }
        else
        {
            skinned = p0;
        }
    }
    vec4 drawP = boneData[0];
    vec4 viewP = metadata;
    vec2 px = vec2(drawP.x + skinned.x * drawP.z,
                   drawP.y + skinned.y * drawP.z);
    vec2 ndc = vec2(px.x / viewP.x * 2.0 - 1.0,
                    1.0 - px.y / viewP.y * 2.0);
    outData[vid] = vec4(ndc, uv);
}
