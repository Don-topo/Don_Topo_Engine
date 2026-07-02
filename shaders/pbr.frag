#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec3 fragWorldPos;
layout(location = 4) in vec3 fragTangent;
layout(location = 5) in vec3 fragBitangent;
layout(location = 6) in vec4 fragLightSpacePos;

layout(location = 0) out vec4 outColor;

#define MAX_LIGHTS 4
struct Light { vec4 position; vec4 color; };

layout(set = 0, binding = 0) uniform UBO {
    mat4  view;
    mat4  proj;
    mat4  lightSpaceMatrix;
    Light lights[MAX_LIGHTS];
    vec4  viewPos;
    int   numLights;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D texSampler;
layout(set = 0, binding = 2) uniform sampler2D normalMap;
layout(set = 0, binding = 3) uniform sampler2DShadow shadowMap;
layout(set = 0, binding = 4) uniform sampler2D metallicRoughnessTex;

layout(push_constant) uniform PushData {
    mat4  transform;
    float metallic;
    float roughness;
    vec2  _pad;
} push;

const float PI = 3.14159265359;

float computeShadow(vec4 lightSpacePos)
{
    vec3 proj = lightSpacePos.xyz / lightSpacePos.w;
    proj.xy   = proj.xy * 0.5 + 0.5;
    if (proj.z > 1.0 || proj.z < 0.0) return 1.0;
    vec2 texelSize = 1.0 / vec2(2048.0);
    float shadow = 0.0;
    for (int x = -1; x <= 1; x++)
        for (int y = -1; y <= 1; y++)
            shadow += texture(shadowMap, vec3(proj.xy + vec2(x, y) * texelSize, proj.z));
    return shadow / 9.0;
}

vec3 aces(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    // TBN + normal map
    mat3 TBN = mat3(fragTangent, fragBitangent, fragNormal);
    vec3 N   = normalize(TBN * (texture(normalMap, fragUV).rgb * 2.0 - 1.0));
    vec3 V   = normalize(ubo.viewPos.xyz - fragWorldPos);

    // Albedo — VK_FORMAT_R8G8B8A8_SRGB ya lineariza en hardware, no aplicar pow de nuevo
    vec4 albedoSample = texture(texSampler, fragUV);
    if (albedoSample.a < 0.5) discard;
    vec3 albedo = albedoSample.rgb;

    // ORM: R=AO, G=roughness, B=metallic — multiply by push scalars
    vec3  orm   = texture(metallicRoughnessTex, fragUV).rgb;
    float ao    = orm.r;
    float rough = clamp(orm.g * push.roughness, 0.04, 1.0);
    float metal = clamp(orm.b * push.metallic,  0.0,  1.0);

    vec3 F0 = mix(vec3(0.04), albedo, metal);

    float shadow = computeShadow(fragLightSpacePos);
    vec3  Lo     = vec3(0.0);

    for (int i = 0; i < ubo.numLights; i++)
    {
        vec3  L        = normalize(ubo.lights[i].position.xyz - fragWorldPos);
        vec3  H        = normalize(V + L);
        vec3  radiance = ubo.lights[i].color.rgb * ubo.lights[i].color.a;

        float NdotL = max(dot(N, L), 0.0);
        float NdotV = max(dot(N, V), 0.0);
        float NdotH = max(dot(N, H), 0.0);
        float HdotV = max(dot(H, V), 0.0);

        // D — GGX distribution
        float a  = rough * rough;
        float a2 = a * a;
        float d  = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
        float D  = a2 / (PI * d * d);

        // G — Smith-Schlick-GGX
        float r = rough + 1.0;
        float k = (r * r) / 8.0;
        float G = (NdotV / (NdotV * (1.0 - k) + k)) * (NdotL / (NdotL * (1.0 - k) + k));

        // F — Schlick
        vec3 F  = F0 + (1.0 - F0) * pow(clamp(1.0 - HdotV, 0.0, 1.0), 5.0);

        vec3 kD = (1.0 - F) * (1.0 - metal);

        float s = (i == 0) ? shadow : 1.0;

        Lo += s * (kD * albedo / PI + D * G * F / (4.0 * NdotV * NdotL + 0.0001))
              * radiance * NdotL;
    }

    float upFactor = N.y * 0.5 + 0.5;
    vec3 skyColor    = vec3(0.10, 0.12, 0.15);
    vec3 groundColor = vec3(0.05, 0.04, 0.03);
    vec3 ambient = mix(groundColor, skyColor, upFactor) * albedo * ao;
    vec3 color   = ambient + Lo;

    outColor = vec4(pow(aces(color), vec3(1.0 / 2.2)), 1.0);
}
