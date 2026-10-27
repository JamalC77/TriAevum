#version 450
#extension GL_EXT_multiview : require

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(set = 0, binding = 0) uniform Scene { mat4 viewProj[2]; } scene;
layout(push_constant) uniform Push { mat4 model; vec4 color; vec4 params; } pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec3 vWorld;

void main() {
    vec4 world = pc.model * vec4(inPos, 1.0);
    vWorld = world.xyz;
    vNormal = mat3(pc.model) * inNormal;
    gl_Position = scene.viewProj[gl_ViewIndex] * world;
}
