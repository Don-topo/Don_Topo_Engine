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

float computeShadow(vec4 lightSpacePos)
{
    vec3 proj   = lightSpacePos.xyz / lightSpacePos.w;
    proj.xy     = proj.xy * 0.5 + 0.5;
    if(proj.z > 1.0 || proj.z < 0.0) return 1.0;

    vec2 texelSize = 1.0 / vec2(2048.0);
    float shadow = 0.0;
    for (int x = -1; x <= 1; x++)
        for (int y = -1; y <= 1; y++)
            shadow += texture(shadowMap, vec3(proj.xy + vec2(x, y) * texelSize, proj.z));
    return shadow / 9.0;
}

void main()
{
    mat3 TBN  = mat3(fragTangent, fragBitangent, fragNormal);
    vec3 norm = texture(normalMap, fragUV).rgb * 2.0 - 1.0;
    norm      = normalize(TBN * norm);

    vec3 viewDir    = normalize(ubo.viewPos.xyz - fragWorldPos);
    vec3 texColor   = texture(texSampler, fragUV).rgb;
    float shadow    = computeShadow(fragLightSpacePos);

    vec3 result = 0.1 * texColor; // ambient global

    for (int i = 0; i < ubo.numLights; i++)
    {
        vec3 lightDir   = normalize(ubo.lights[i].position.xyz - fragWorldPos);
        float diff      = max(dot(norm, lightDir), 0.0);
        vec3 reflDir    = reflect(-lightDir, norm);
        float spec      = pow(max(dot(viewDir, reflDir), 0.0), 32.0);
        vec3 lightColor = ubo.lights[i].color.rgb * ubo.lights[i].color.a;

        float s = (i == 0) ? shadow : 1.0; // solo key light proyecta sombra
        result += s * (diff * texColor * lightColor + 0.3 * spec * lightColor);
    }

    outColor = vec4(result, 1.0);
}