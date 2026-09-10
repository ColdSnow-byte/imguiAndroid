#version 450

// Vertex layout matches BuildSolidMesh(): position(3) + normal(3) + color(3).
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;

// Two matrices passed without any descriptor set: Vulkan guarantees at least
// 128 bytes of push constants, which is exactly two mat4s.
layout(push_constant) uniform PushConstants
{
    mat4 mvp;
    mat4 model;
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vColor;

void main()
{
    gl_Position = pc.mvp * vec4(aPosition, 1.0);
    vNormal     = mat3(pc.model) * aNormal;
    vColor      = aColor;
}
