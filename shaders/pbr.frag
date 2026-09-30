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
    // It goes in the padding gap that was already behind numLights, so
    // no previous offset moves and the other 4 shaders that declare this
    // block do not need to change.
    float ambientIntensity;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D texSampler;
layout(set = 0, binding = 2) uniform sampler2D normalMap;
layout(set = 0, binding = 3) uniform sampler2DArrayShadow shadowMap;
layout(set = 0, binding = 4) uniform sampler2D metallicRoughnessTex;
// IBL. The two cubemaps ALWAYS exist: without a loaded skybox they carry a constant
// neutral ambient, so no branch is needed here.
layout(set = 0, binding = 5) uniform samplerCube irradianceMap;
layout(set = 0, binding = 6) uniform samplerCube prefilterMap;
// The frame's SSAO, at full resolution and already blurred. It ALWAYS exists: with
// the effect off the image is set to 1.0 and this shader multiplies by
// one, so no branch or new UBO member is needed.
layout(set = 0, binding = 7) uniform sampler2D ssaoMap;

// ── Forward+ ────────────────────────────────────────────────────────────────
// Its own set 2 and not new bindings in set 0: set 0 only had 8 free and
// extending it would force rewriting the descriptor set of EVERY object. This set
// is one per frame and is bound once per pass. The buffers ALWAYS EXIST,
// also with Forward+ off: then fp.mode is 0 and the loop below is
// the usual one over the UBO, without reading a single light from here.
struct FpLight
{
    vec4 posRadius;     // xyz world, w radius
    vec4 color;         // rgb color, a intensity
    vec4 viewPosR;      // xyz view space, w radio (solo lo usa el culling)
    vec4 direction;     // xyz dir, w type (0 point, 1 spot, 2 directional, 3 area)
    vec4 params;        // range, inner cos, outer cos, width
};

layout(std430, set = 2, binding = 0) readonly buffer FpParamsBuf {
    uint  mode;         // 0 off, 1 tiled, 2 clustered
    uint  gridX;
    uint  gridY;
    uint  gridZ;
    uint  tileSize;
    uint  maxPerCell;
    uint  numLights;
    uint  pad0;
    float zNear;
    float zFar;
    float sliceScale;
    float sliceBias;
} fp;

layout(std430, set = 2, binding = 1) readonly buffer FpLightBuf { FpLight fpLights[];  };
layout(std430, set = 2, binding = 2) readonly buffer FpGridBuf  { uvec2   fpCells[];   };
layout(std430, set = 2, binding = 3) readonly buffer FpIndexBuf { uint    fpIndices[]; };

// Must match Renderer::IBL_PREFILTER_MIPS. It is a #define and not in the
// UBO on purpose: the UBO block is declared in 5 shaders and adding a
// member would silently shift everything behind it under std140.
#define IBL_PREFILTER_MIPS 5

layout(push_constant) uniform PushData {
    mat4  transform;
    float metallic;
    float roughness;
    // flags.x: instancing path, read by the vertex shader.
    // flags.y: the object's SSR strength, which this shader writes to the alpha of the
    // HDR attachment. It is the channel through which the per-object mask reaches the
    // reflection post-pass without a new attachment or a UBO member.
    vec2  flags;
} push;

const float PI = 3.14159265359;

// Direction towards the light and attenuation according to its type. The identical copy of this
// function lives in triangle.frag: if the two stop matching, the same object
// looks different depending on whether it has a PBR material or not.
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
    // Same radius window as the Forward+ branch below: outside the range it gives
    // EXACTLY 0, so discarding the light does not change the result.
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

// The layer choice and the reprojection are shared with fog.comp, which samples
// the SAME map; only the filtering remains here, which is different on purpose.
#include "shadow_lookup.glsl"

// normalGeo = the vertex's INTERPOLATED normal, not the normal map's: the bias
// only has to separate the surface from its own shadow, and making it follow the
// bumps of a texture puts ripples on the shadow's edge.
// 3x3 PCF over an already resolved coordinate. The key light and the secondary spots
// share it: the filtering has to be the same or the same geometry would give
// different edges depending on which light shades it.
float dtPcf(vec3 proj, float layer)
{
    // From the map's REAL size, not a hardcoded 2048. With the fixed value, raising
    // the resolution neither widened nor narrowed the filter: at 4096 the nine taps
    // were two real texels apart (same blur, just less aliasing) and
    // at 1024 they fell within half a texel and the PCF disappeared.
    vec2  texelSize = 1.0 / vec2(textureSize(shadowMap, 0).xy);
    float shadow    = 0.0;
    // 5x5 was tried for the edge of a perspective shadow and it looks WORSE: with
    // a cubemap the extra taps are clamped against the face's edge and
    // widen that hard band. Since it is indexed by layer and not by region of an atlas,
    // the edge taps cannot fall in the neighboring layer: the sampler
    // clamps them against the edge of THEIR layer.
    for (int x = -1; x <= 1; x++)
        for (int y = -1; y <= 1; y++)
            shadow += texture(shadowMap, vec4(proj.xy + vec2(x, y) * texelSize, layer, proj.z));
    return shadow / 9.0;
}

// Shadow of a light that is not the key. 1.0 = lit, which is what it returns
// also when that light casts no shadow: the vast majority of the lights in
// a scene have no slot, and for them this is a comparison and an exit.
float shadowDeLuz(int luz, vec3 worldPos, vec3 normalGeo)
{
    vec3  proj;
    float layer;
    if (!dtShadowCoordExtra(luz, worldPos, normalGeo, proj, layer)) return 1.0;
    return dtPcf(proj, layer);
}

float computeShadow(vec3 worldPos, vec3 normalGeo)
{
    // It is reprojected here instead of bringing N varyings from the vertex shader: the
    // cascade is not known until the fragment's depth is available.
    vec3  proj;
    float layer;
    if (!dtShadowCoord(worldPos, normalGeo, proj, layer)) return 1.0;

    return dtPcf(proj, layer);
}

// Schlick's Fresnel with Lazarov's roughness term: without it, a
// rough surface seen edge-on would return kS = 1 and be left without diffuse.
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float rough)
{
    return F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Second half of the split-sum (the BRDF integral), in Karis's analytic form.
// It replaces the 512x512 2D LUT with an error of the order of 1%, and
// saves an image, a binding and a precomputation pass.
vec3 envBRDFApprox(vec3 F0, float rough, float NdotV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572,  0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.040, -0.040);
    vec4  r    = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2  ab   = vec2(-1.04, 1.04) * a004 + r.zw;
    return F0 * ab.x + ab.y;
}

void main()
{
    // TBN + normal map
    mat3 TBN = mat3(fragTangent, fragBitangent, fragNormal);
    vec3 N   = normalize(TBN * (texture(normalMap, fragUV).rgb * 2.0 - 1.0));
    vec3 V   = normalize(ubo.viewPos.xyz - fragWorldPos);

    // Albedo — VK_FORMAT_R8G8B8A8_SRGB already linearizes in hardware, do not apply pow again
    vec4 albedoSample = texture(texSampler, fragUV);
    if (albedoSample.a < 0.5) discard;
    vec3 albedo = albedoSample.rgb;

    // ORM: R=AO, G=roughness, B=metallic — multiply by push scalars
    vec3  orm   = texture(metallicRoughnessTex, fragUV).rgb;
    float ao    = orm.r;
    float rough = clamp(orm.g * push.roughness, 0.04, 1.0);
    float metal = clamp(orm.b * push.metallic,  0.0,  1.0);

    vec3 F0 = mix(vec3(0.04), albedo, metal);

    // Depth in view space. The shadow no longer uses it (the layer choice
    // lives in shadow_lookup.glsl and recomputes it there) but the split into
    // slices of clustered Forward+ does.
    float viewDepth = -(ubo.view * vec4(fragWorldPos, 1.0)).z;
    float shadow    = computeShadow(fragWorldPos, normalize(fragNormal));
    vec3  Lo        = vec3(0.0);

    // Forward+ off: the usual loop over the UBO's MAX_LIGHTS, without
    // touching a single buffer of set 2. It is copied and not factored with the one
    // below on purpose: they are the same operations in the same order, and the
    // only difference is where each light comes from.
    if (fp.mode == 0u)
    {
    for (int i = 0; i < ubo.numLights; i++)
    {
        vec3  L   = vec3(0.0);
        float att = lightSample(i, fragWorldPos, L);
        if (att <= 0.0) continue;

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

        // The key light uses the shadow already computed above; the others, their own if they
        // got a slot. A light without a slot returns 1.0 without sampling.
        float s = (i == 0) ? shadow : shadowDeLuz(i, fragWorldPos, normalize(fragNormal));

        Lo += att * s * (kD * albedo / PI + D * G * F / (4.0 * NdotV * NdotL + 0.0001))
              * radiance * NdotL;
    }
    }
    else
    {
        // Cell of this fragment. gl_FragCoord is in target pixels, which is
        // the INTERNAL resolution — the same one the grid was sized with.
        uvec2 tile = uvec2(gl_FragCoord.xy) / fp.tileSize;
        tile = min(tile, uvec2(fp.gridX - 1u, fp.gridY - 1u));

        uint cell;
        if (fp.mode == 1u)
        {
            cell = tile.y * fp.gridX + tile.x;
        }
        else
        {
            // Exact inverse of the logarithmic split of light_cull_clustered.comp.
            float sl = log2(max(viewDepth, fp.zNear)) * fp.sliceScale + fp.sliceBias;
            uint slice = uint(clamp(sl, 0.0, float(fp.gridZ - 1u)));
            cell = (slice * fp.gridY + tile.y) * fp.gridX + tile.x;
        }

        uvec2 cellData = fpCells[cell];
        for (uint c = 0u; c < cellData.y; c++)
        {
            uint  li = fpIndices[cellData.x + c];
            vec3  lp = fpLights[li].posRadius.xyz;
            float lr = fpLights[li].posRadius.w;
            int   lt = int(fpLights[li].direction.w + 0.5);

            vec3  toL  = lp - fragWorldPos;
            float dist = length(toL);
            // Radius window: outside the radius it gives EXACTLY 0, which is what
            // makes putting extra lights in a cell not change the result,
            // and therefore makes tiled and clustered, which cull with different
            // volumes, give the same image.
            float w   = clamp(1.0 - (dist * dist) / (lr * lr), 0.0, 1.0);
            float att = w * w;

            vec3  L = toL / max(dist, 1e-4);

            // Directional: no position or attenuation. It goes apart from the radius one
            // above because the binning puts it in ALL the cells.
            if (lt == 2)
            {
                L   = normalize(-fpLights[li].direction.xyz);
                att = 1.0;
            }
            else if (lt == 1)
            {
                // Spot: same soft cone as lightSample().
                float cosA = dot(normalize(fpLights[li].direction.xyz), -L);
                att *= smoothstep(fpLights[li].params.z, fpLights[li].params.y, cosA);
            }
            if (att <= 0.0) continue;

            vec3  H        = normalize(V + L);
            vec3  radiance = fpLights[li].color.rgb * fpLights[li].color.a;

            float NdotL = max(dot(N, L), 0.0);
            float NdotV = max(dot(N, V), 0.0);
            float NdotH = max(dot(N, H), 0.0);
            float HdotV = max(dot(H, V), 0.0);

            float a  = rough * rough;
            float a2 = a * a;
            float d  = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
            float D  = a2 / (PI * d * d);

            float r = rough + 1.0;
            float k = (r * r) / 8.0;
            float G = (NdotV / (NdotV * (1.0 - k) + k)) * (NdotL / (NdotL * (1.0 - k) + k));

            vec3 F  = F0 + (1.0 - F0) * pow(clamp(1.0 - HdotV, 0.0, 1.0), 5.0);

            vec3 kD = (1.0 - F) * (1.0 - metal);

            // The index being compared is the GLOBAL one, not the cell's: the
            // shadow slot is allocated over the UBO's light array.
            float s = (li == 0u) ? shadow
                                 : shadowDeLuz(int(li), fragWorldPos, normalize(fragNormal));

            Lo += s * (kD * albedo / PI + D * G * F / (4.0 * NdotV * NdotL + 0.0001))
                  * radiance * NdotL * att;
        }
    }

    // ── Ambient: IBL ───────────────────────────────────────────────────────
    float NdotVamb = max(dot(N, V), 0.0);
    vec3  Famb     = fresnelSchlickRoughness(NdotVamb, F0, rough);
    // A metal has no diffuse, and the specular it reflects it does not transmit.
    vec3  kDamb    = (1.0 - Famb) * (1.0 - metal);

    // The cubemap already stores E/PI, so the 1/PI of the Lambertian BRDF is not
    // applied again here.
    vec3 diffuseIBL = texture(irradianceMap, N).rgb * albedo;

    // Roughness picks the mip: the last one is the widest lobe.
    vec3 R           = reflect(-V, N);
    vec3 prefiltered = textureLod(prefilterMap, R, rough * float(IBL_PREFILTER_MIPS - 1)).rgb;
    vec3 specularIBL = prefiltered * envBRDFApprox(F0, rough, NdotVamb);

    // The multiplier scales diffuse and specular equally: it raises or lowers the weight
    // of the environment without changing its color or the balance between the two terms.
    // SSAO goes in HERE and not on the final color: it is environment occlusion, and
    // applying it to the direct light too would turn off shadows that the
    // shadow map already computes. It is sampled by screen coordinate; the map is the exact
    // size of the framebuffer, so the division is 1:1 and there is no need to carry
    // the resolution anywhere.
    float ssao   = texture(ssaoMap, gl_FragCoord.xy / vec2(textureSize(ssaoMap, 0))).r;
    vec3 ambient = (kDamb * diffuseIBL + specularIBL) * ao * ssao * ubo.ambientIntensity;
    vec3 color   = ambient + Lo;

    // Not tonemapped: this pass's attachment is R16G16B16A16_SFLOAT and it is
    // consumed by the bloom chain, which needs the high range intact. The ACES +
    // gamma that was here now lives in shaders/bloom_composite.frag, which is the
    // only place in the engine where HDR becomes LDR.
    // The alpha carries the object's SSR strength, not opacity: ssr.comp reads it
    // as a per-pixel mask. Before this feature it was 1.0 and nobody read it
    // (bloom_composite.frag and bloom_down.comp only use .rgb), so with SSR
    // disabled the image comes out exactly the same.
    outColor = vec4(color, push.flags.y);
}
