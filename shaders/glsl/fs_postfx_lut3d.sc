// Caesura (AmeKAG) - 3D LUT color-grade FS (GLSL, bgfx shaderc, t214)
$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_texture, 0);
SAMPLER2D(s_lut, 1);
uniform vec4 PostFxParams[4]; // [0]=intensity [2]=(1/w,1/h,N,0)

// Packed x=b*N+r, y=g; explicit interpolation preserves the contract even
// when a caller-created LUT uses point sampling.
vec3 lutCell(float r, float g, float b, float N) {
    vec2 uv = vec2((b * N + r + 0.5) / (N * N), (g + 0.5) / N);
    return texture2DLod(s_lut, uv, 0.0).rgb;
}
vec3 lutSlice(vec2 rg, float b, float N) {
    vec2 lo = floor(rg);
    vec2 hi = min(lo + vec2(1.0), vec2(N - 1.0));
    vec2 weight = rg - lo;
    return mix(mix(lutCell(lo.x, lo.y, b, N), lutCell(hi.x, lo.y, b, N), weight.x),
               mix(lutCell(lo.x, hi.y, b, N), lutCell(hi.x, hi.y, b, N), weight.x), weight.y);
}
#if BGFX_SHADER_LANGUAGE_GLSL
out vec4 bgfx_FragColor;
#endif
void main() {
    vec4 src = texture2D(s_texture, v_texcoord0);
    float N = max(floor(PostFxParams[2].z + 0.5), 2.0);
    vec3 coordinate = clamp(src.rgb, 0.0, 1.0) * (N - 1.0);
    float lowB = floor(coordinate.b);
    float highB = min(lowB + 1.0, N - 1.0);
    vec3 lut = mix(lutSlice(coordinate.rg, lowB, N), lutSlice(coordinate.rg, highB, N), coordinate.b - lowB);
    vec3 outC = mix(src.rgb, lut, clamp(PostFxParams[0].x, 0.0, 1.0));
    #if BGFX_SHADER_LANGUAGE_GLSL

    bgfx_FragColor = vec4(outC, src.a);

    #else

    gl_FragColor = vec4(outC, src.a);

    #endif
}
