#version 450

// Paso final: escena HDR + bloom, y AQUI es donde se tonemapea. La formula es
// literalmente la ultima linea que tenia pbr.frag (ACES + gamma 2.2), asi que
// con intensity = 0 el resultado es el mismo byte a byte que antes de la feature.
layout(location = 0) in  vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sceneHdr;
layout(set = 0, binding = 1) uniform sampler2D bloomTex;

layout(push_constant) uniform Push {
    float intensity;
} push;

vec3 aces(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    vec3 color = texture(sceneHdr, inUv).rgb;
    color += texture(bloomTex, inUv).rgb * push.intensity;
    outColor = vec4(pow(aces(color), vec3(1.0 / 2.2)), 1.0);
}
