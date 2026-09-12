// Straight-alpha texture color. Keep fs_texture as the unmodulated copy path.
$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_texture, 0);
uniform vec4 u_color;
#if BGFX_SHADER_LANGUAGE_GLSL
out vec4 bgfx_FragColor;
#endif
void main() {
    vec4 color = texture2D(s_texture, v_texcoord0) * u_color;
#if BGFX_SHADER_LANGUAGE_GLSL
    bgfx_FragColor = color;
#else
    gl_FragColor = color;
#endif
}
