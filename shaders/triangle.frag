#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec3 fragWorldPos;
layout(location = 4) in vec3 fragTangent;
layout(location = 5) in vec3 fragBitangent;

layout(location = 0) out vec4 outColor;


layout(set = 0, binding = 0) uniform UBO {
    mat4 view;
    mat4 proj;
    vec4 lightPos;
    vec4 viewPos;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D texSampler;
layout(set = 0, binding = 2) uniform sampler2D normalMap;

void main()
{
    mat3 TBN      = mat3(fragTangent, fragBitangent, fragNormal);
    vec3 norm     = texture(normalMap, fragUV).rgb * 2.0 - 1.0;
    norm          = normalize(TBN * norm);
    vec3 lightDir = normalize(ubo.lightPos.xyz - fragWorldPos);
    float diff    = max(dot(norm, lightDir), 0.0);

    vec3 viewDir  = normalize(ubo.viewPos.xyz - fragWorldPos);
    vec3 reflDir  = reflect(-lightDir, norm);
    float spec    = pow(max(dot(viewDir, reflDir), 0.0), 32.0);

    vec3 ambient  = 0.15 * texture(texSampler, fragUV).rgb;
    vec3 diffuse  = diff * texture(texSampler, fragUV).rgb;
    vec3 specular = 0.3 * spec * vec3(1.0);

    outColor = vec4(ambient + diffuse + specular, 1.0);
}