#version 450

#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0, std430) readonly buffer Aux
{
    uint descriptor_index_bias;
} aux_buffer;

layout(set = 0, binding = 1) uniform texture2D source_textures[];
layout(set = 1, binding = 0) uniform sampler source_sampler;

layout(push_constant) uniform Parameters
{
    uint descriptor_index;
} parameters;

layout(location = 0) out vec4 output_color;

void main()
{
    uint descriptor_index = parameters.descriptor_index +
                            aux_buffer.descriptor_index_bias;
    output_color = texture(
        sampler2D(source_textures[nonuniformEXT(descriptor_index)],
                  source_sampler),
        vec2(0.5));
}
