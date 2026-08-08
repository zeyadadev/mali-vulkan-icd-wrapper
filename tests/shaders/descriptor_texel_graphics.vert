#version 450

#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform utextureBuffer source_uint_buffers[];
layout(set = 0, binding = 0) uniform textureBuffer source_float_buffers[];

layout(push_constant) uniform Parameters
{
    uint uint_descriptor_index;
    uint float_descriptor_index;
} parameters;

layout(location = 0) flat out uvec2 vertex_values;

const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0));

void main()
{
    vertex_values.x = texelFetch(
        source_uint_buffers[parameters.uint_descriptor_index], 0).x;
    vertex_values.y = floatBitsToUint(texelFetch(
        source_float_buffers[parameters.float_descriptor_index], 0).x);
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
