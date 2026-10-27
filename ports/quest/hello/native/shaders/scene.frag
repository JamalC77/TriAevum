#version 450

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec3 vWorld;

layout(push_constant) uniform Push { mat4 model; vec4 color; vec4 params; } pc;

layout(location = 0) out vec4 outColor;

void main() {
    if (pc.params.x > 0.5) {
        // Grid floor: 0.5 m cells, anti-aliased lines, fading out at 8 m.
        vec2 p = vWorld.xz * 2.0;
        vec2 g = abs(fract(p - 0.5) - 0.5) / max(fwidth(p), vec2(1e-4));
        float line = 1.0 - min(min(g.x, g.y), 1.0);
        float fade = clamp(1.0 - length(vWorld.xz - pc.params.yz) / 8.0, 0.0, 1.0);
        vec3 base = vec3(0.02, 0.025, 0.035);
        outColor = vec4(mix(base, vec3(0.20, 0.55, 0.85), line * fade), 1.0);
        return;
    }
    vec3 n = normalize(vNormal);
    vec3 l = normalize(vec3(0.4, 1.0, 0.3));
    float diff = max(dot(n, l), 0.0);
    outColor = vec4(pc.color.rgb * (0.22 + 0.78 * diff), 1.0);
}
