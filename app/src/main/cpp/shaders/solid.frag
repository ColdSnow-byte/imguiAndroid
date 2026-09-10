#version 450

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vColor;

layout(location = 0) out vec4 outColor;

// Lighting is intentionally identical to the GLSL ES version in
// platonic_solid.cpp and to the software rasterizer in cpu_solid.cpp, so all
// three backends produce the same picture.
const vec3  LIGHT_DIR     = vec3(0.35, 0.75, 0.85);
const vec3  RIM_COLOR     = vec3(0.30, 0.45, 0.65);
const float AMBIENT       = 0.40;
const float DIFFUSE_GAIN  = 0.75;
const float RIM_GAIN      = 0.45;

void main()
{
    vec3  n       = normalize(vNormal);
    vec3  l       = normalize(LIGHT_DIR);
    float diffuse = max(dot(n, l), 0.0);
    float rim     = pow(1.0 - max(dot(n, vec3(0.0, 0.0, 1.0)), 0.0), 2.0);

    vec3 c = vColor * (AMBIENT + DIFFUSE_GAIN * diffuse) + RIM_COLOR * rim * RIM_GAIN;
    outColor = vec4(c, 1.0);
}
