#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec3 fragWorldPos;
layout(location = 4) in vec3 fragTangent;
layout(location = 5) in vec3 fragBitangent;

layout(location = 0) out vec4 outColor;

#include "lights_config.glsl"
#include "shadow_config.glsl"
// Shadow matrix slots. The first 6 belong to the KEY light (4 cascades,
// or 6 cubemap faces, or 1 spot face); the 4 after them are one secondary
// spot each. Same value as SHADOW_MATRICES in
// UniformBufferObject.h.
// Same layout as DonTopo::Light. direction.w = type (0 point, 1 spot,
// 2 directional, 3 area); params = (range, inner cos, outer cos, width).
struct Light { vec4 position; vec4 color; vec4 direction; vec4 params; };

layout(set = 0, binding = 0) uniform UBO {
    mat4  view;
    mat4  proj;
    mat4  lightSpaceMatrix[SHADOW_MATRICES];
    vec4  cascadeSplits;    // distance (view space, positive) that each cascade reaches
    Light lights[MAX_LIGHTS];
    vec4  viewPos;
    int   numLights;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D texSampler;
layout(set = 0, binding = 2) uniform sampler2D normalMap;
layout(set = 0, binding = 3) uniform sampler2DArrayShadow shadowMap;

// Direction towards the light and attenuation according to its type. Identical to pbr.frag's:
// if the two stop matching, the same object looks different depending on whether it has a
// PBR material or not.
float lightSample(int i, vec3 worldPos, out vec3 L)
{
    int type = int(ubo.lights[i].direction.w + 0.5);

    // Directional: no position or attenuation, only direction.
    if (type == 2)
    {
        L = normalize(-ubo.lights[i].direction.xyz);
        return 1.0;
    }

    vec3  toL  = ubo.lights[i].position.xyz - worldPos;
    float dist = length(toL);
    L = toL / max(dist, 1e-4);

    // The area light is approximated as a point of radius = width/2.
    float range = (type == 3) ? max(ubo.lights[i].params.w * 0.5, 1e-4)
                              : max(ubo.lights[i].params.x, 1e-4);
    // Same radius window as the Forward+ binning: outside the range it gives
    // EXACTLY 0, which is what allows discarding the light without it being noticeable.
    float w   = clamp(1.0 - (dist * dist) / (range * range), 0.0, 1.0);
    float att = w * w;

    // Spot: soft cone between the inner and the outer cosine.
    if (type == 1)
    {
        float cosA = dot(normalize(ubo.lights[i].direction.xyz), -L);
        att *= smoothstep(ubo.lights[i].params.z, ubo.lights[i].params.y, cosA);
    }
    return att;
}

// Same selection and same PCF as pbr.frag; see there why it reprojects
// from the world position instead of bringing it from the vertex shader.
int selectCascade(float viewDepth)
{
    for (int i = 0; i < SHADOW_CASCADES; i++)
        if (viewDepth <= ubo.cascadeSplits[i]) return i;
    return -1;
}

float computeShadow(vec3 worldPos, int cascade)
{
    vec4 lightSpacePos = ubo.lightSpaceMatrix[cascade] * vec4(worldPos, 1.0);
    vec3 proj   = lightSpacePos.xyz / lightSpacePos.w;
    proj.xy     = proj.xy * 0.5 + 0.5;
    if(proj.z > 1.0 || proj.z < 0.0) return 1.0;

    vec2 texelSize = 1.0 / vec2(2048.0);
    float shadow = 0.0;
    for (int x = -1; x <= 1; x++)
        for (int y = -1; y <= 1; y++)
            shadow += texture(shadowMap, vec4(proj.xy + vec2(x, y) * texelSize, float(cascade), proj.z));
    return shadow / 9.0;
}

void main()
{
    mat3 TBN  = mat3(fragTangent, fragBitangent, fragNormal);
    vec3 norm = texture(normalMap, fragUV).rgb * 2.0 - 1.0;
    norm      = normalize(TBN * norm);

    vec3 viewDir    = normalize(ubo.viewPos.xyz - fragWorldPos);
    vec3 texColor   = texture(texSampler, fragUV).rgb;
    float viewDepth = -(ubo.view * vec4(fragWorldPos, 1.0)).z;
    int   cascade   = selectCascade(viewDepth);
    float shadow    = cascade < 0 ? 1.0 : computeShadow(fragWorldPos, cascade);

    vec3 result = 0.1 * texColor; // ambient global

    for (int i = 0; i < ubo.numLights; i++)
    {
        vec3  lightDir  = vec3(0.0);
        float att       = lightSample(i, fragWorldPos, lightDir);
        if (att <= 0.0) continue;

        float diff      = max(dot(norm, lightDir), 0.0);
        vec3 reflDir    = reflect(-lightDir, norm);
        float spec      = pow(max(dot(viewDir, reflDir), 0.0), 32.0);
        vec3 lightColor = ubo.lights[i].color.rgb * ubo.lights[i].color.a;

        float s = (i == 0) ? shadow : 1.0; // only the key light casts shadow
        result += att * s * (diff * texColor * lightColor + 0.3 * spec * lightColor);
    }

    outColor = vec4(result, 1.0);
}