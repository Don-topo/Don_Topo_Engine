#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec3 fragWorldPos;
layout(location = 4) in vec3 fragTangent;
layout(location = 5) in vec3 fragBitangent;

layout(location = 0) out vec4 outColor;

#define MAX_LIGHTS 4
struct Light { vec4 position; vec4 color; };

layout(set = 0, binding = 0) uniform UBO {
    mat4  view;
    mat4  proj;
    Light lights[MAX_LIGHTS];
    vec4  viewPos;
    int   numLights;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D texSampler;
layout(set = 0, binding = 2) uniform sampler2D normalMap;

void main()
{
    mat3 TBN  = mat3(fragTangent, fragBitangent, fragNormal);
    vec3 norm = texture(normalMap, fragUV).rgb * 2.0 - 1.0;
    norm      = normalize(TBN * norm);

    vec3 viewDir  = normalize(ubo.viewPos.xyz - fragWorldPos);
    vec3 texColor = texture(texSampler, fragUV).rgb;

    vec3 result = 0.1 * texColor; // ambient global

    for (int i = 0; i < ubo.numLights; i++)
    {
        vec3 lightDir   = normalize(ubo.lights[i].position.xyz - fragWorldPos);
        float diff      = max(dot(norm, lightDir), 0.0);
        vec3 reflDir    = reflect(-lightDir, norm);
        float spec      = pow(max(dot(viewDir, reflDir), 0.0), 32.0);
        vec3 lightColor = ubo.lights[i].color.rgb * ubo.lights[i].color.a;

        result += diff * texColor * lightColor;
        result += 0.3 * spec * lightColor;
    }

    outColor = vec4(result, 1.0);
}