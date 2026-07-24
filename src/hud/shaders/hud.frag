#version 400
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

layout(set = 0, binding = 0) uniform sampler2D font_atlas;
layout(location = 0) in vec2 fragment_uv;
layout(location = 1) in vec4 fragment_color;
layout(location = 0) out vec4 out_color;

void main()
{
    float distance = texture(font_atlas, fragment_uv).r;
    float coverage = smoothstep(0.44, 0.56, distance);
    out_color = vec4(fragment_color.rgb, fragment_color.a * coverage);
}
