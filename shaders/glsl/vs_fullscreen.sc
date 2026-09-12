// Caesura (AmeKAG) - Fullscreen Quad Vertex Shader (GLSL)
// Standard NDC POSITION/TEXCOORD quad, matching the production draw buffers.
// Corresponds to shaders/dx11/vs_fullscreen.hlsl

$input a_position, a_texcoord0
$output v_texcoord0

#include <bgfx_shader.sh>

void main()
{
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_texcoord0 = a_texcoord0;
}
