#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragNormal;
layout(location = 3) in vec3 fragWorldPos;
layout(location = 4) in vec3 fragTangent;
layout(location = 5) in vec3 fragBitangent;

layout(location = 0) out vec4 outColor;

#define MAX_LIGHTS 16
#define SHADOW_CASCADES 4
// Huecos de matriz de sombra: 4 cascadas de una direccional o 6 caras del
// cubemap de una de punto. Nunca coexisten. Mismo valor que
// SHADOW_MATRICES en UniformBufferObject.h.
#define SHADOW_MATRICES 6
// Mismo layout que DonTopo::Light. direction.w = tipo (0 point, 1 spot,
// 2 directional, 3 area); params = (range, cos interior, cos exterior, ancho).
struct Light { vec4 position; vec4 color; vec4 direction; vec4 params; };

layout(set = 0, binding = 0) uniform UBO {
    mat4  view;
    mat4  proj;
    mat4  lightSpaceMatrix[SHADOW_MATRICES];
    vec4  cascadeSplits;    // distancia (view space, positiva) hasta la que llega cada cascada
    Light lights[MAX_LIGHTS];
    vec4  viewPos;
    int   numLights;
    // Va en el hueco de padding que ya habia detras de numLights, asi que
    // ningun offset anterior se mueve y los otros 4 shaders que declaran este
    // bloque no necesitan cambiar.
    float ambientIntensity;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D texSampler;
layout(set = 0, binding = 2) uniform sampler2D normalMap;
layout(set = 0, binding = 3) uniform sampler2DArrayShadow shadowMap;
layout(set = 0, binding = 4) uniform sampler2D metallicRoughnessTex;
// IBL. Los dos cubemaps existen SIEMPRE: sin skybox cargado llevan un ambiente
// neutro constante, asi que aqui no hace falta ninguna rama.
layout(set = 0, binding = 5) uniform samplerCube irradianceMap;
layout(set = 0, binding = 6) uniform samplerCube prefilterMap;
// SSAO del frame, a resolucion completa y ya emborronado. Existe SIEMPRE: con
// el efecto apagado la imagen esta puesta a 1.0 y este shader multiplica por la
// unidad, asi que no hace falta ninguna rama ni un miembro nuevo en el UBO.
layout(set = 0, binding = 7) uniform sampler2D ssaoMap;

// ── Forward+ ────────────────────────────────────────────────────────────────
// Set 2 propio y no bindings nuevos del set 0: el 0 solo tenia libre el 8 y
// ampliarlo obligaria a reescribir el descriptor set de CADA objeto. Este set
// es uno por frame y se bindea una vez por pass. Los buffers EXISTEN siempre,
// tambien con Forward+ apagado: entonces fp.mode vale 0 y el bucle de abajo es
// el de siempre sobre el UBO, sin leer ni una luz de aqui.
struct FpLight
{
    vec4 posRadius;     // xyz mundo, w radio
    vec4 color;         // rgb color, a intensidad
    vec4 viewPosR;      // xyz view space, w radio (solo lo usa el culling)
    vec4 direction;     // xyz dir, w tipo (0 point, 1 spot, 2 directional, 3 area)
    vec4 params;        // range, cos interior, cos exterior, ancho
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

// Debe coincidir con Renderer::IBL_PREFILTER_MIPS. Va como #define y no en el
// UBO a proposito: el bloque UBO esta declarado en 5 shaders y anadirle un
// miembro desplazaria en silencio todo lo que va detras por std140.
#define IBL_PREFILTER_MIPS 5

layout(push_constant) uniform PushData {
    mat4  transform;
    float metallic;
    float roughness;
    // flags.x: ruta de instancing, la lee el vertex shader.
    // flags.y: fuerza de SSR del objeto, que este shader vuelca al alfa del
    // attachment HDR. Es el canal por el que la mascara por objeto llega al
    // post-pass de reflejos sin un attachment nuevo ni un miembro en el UBO.
    vec2  flags;
} push;

const float PI = 3.14159265359;

// Direccion hacia la luz y atenuacion segun su tipo. La copia identica de esta
// funcion vive en triangle.frag: si las dos dejan de coincidir, el mismo objeto
// se ve distinto segun tenga o no material PBR.
float lightSample(int i, vec3 worldPos, out vec3 L)
{
    int type = int(ubo.lights[i].direction.w + 0.5);

    // Directional: sin posicion ni atenuacion, solo direccion.
    if (type == 2)
    {
        L = normalize(-ubo.lights[i].direction.xyz);
        return 1.0;
    }

    vec3  toL  = ubo.lights[i].position.xyz - worldPos;
    float dist = length(toL);
    L = toL / max(dist, 1e-4);

    // El area se aproxima como un point de radio = ancho/2.
    float range = (type == 3) ? max(ubo.lights[i].params.w * 0.5, 1e-4)
                              : max(ubo.lights[i].params.x, 1e-4);
    // Misma ventana por radio que la rama Forward+ de abajo: fuera del rango da
    // EXACTAMENTE 0, asi que descartar la luz no cambia el resultado.
    float w   = clamp(1.0 - (dist * dist) / (range * range), 0.0, 1.0);
    float att = w * w;

    // Spot: cono suave entre el coseno interior y el exterior.
    if (type == 1)
    {
        float cosA = dot(normalize(ubo.lights[i].direction.xyz), -L);
        att *= smoothstep(ubo.lights[i].params.z, ubo.lights[i].params.y, cosA);
    }
    return att;
}

// La eleccion de capa y la reproyeccion las comparte con fog.comp, que muestrea
// el MISMO mapa; aqui solo queda el filtrado, que si es distinto a proposito.
#include "shadow_lookup.glsl"

// normalGeo = la normal INTERPOLADA del vertice, no la del normal map: el bias
// solo tiene que separar la superficie de su propia sombra, y hacerlo seguir los
// bultos de una textura mete ondulaciones en el borde de la sombra.
float computeShadow(vec3 worldPos, vec3 normalGeo)
{
    // Se reproyecta aqui en vez de traer N varyings del vertex shader: la
    // cascada no se sabe hasta tener la profundidad del fragmento.
    vec3  proj;
    float layer;
    if (!dtShadowCoord(worldPos, normalGeo, proj, layer)) return 1.0;

    // Del tamano REAL del mapa, no de un 2048 a fuego. Con el valor fijo, subir
    // la resolucion no ensanchaba ni estrechaba el filtro: a 4096 los nueve taps
    // se separaban dos texeles reales -mismo desenfoque, solo menos aliasing- y
    // a 1024 caian dentro de medio texel y el PCF desaparecia. Asi el filtro
    // escala con la resolucion, que es lo que hace util el ajuste.
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0).xy);
    float shadow = 0.0;
    // PCF 3x3 dentro de la capa que toque. Se probo 5x5 para el borde de una
    // sombra en perspectiva y se ve PEOR: con un cubemap los taps de mas se
    // recortan contra el borde de la cara y ensanchan esa banda dura.
    // Al indexar por capa y no por region de un atlas, los taps del borde no
    // pueden caer en la capa vecina: el sampler los recorta contra el borde de
    // SU capa.
    for (int x = -1; x <= 1; x++)
        for (int y = -1; y <= 1; y++)
            shadow += texture(shadowMap, vec4(proj.xy + vec2(x, y) * texelSize, layer, proj.z));
    return shadow / 9.0;
}

// Fresnel de Schlick con el termino de rugosidad de Lazarov: sin el, una
// superficie rugosa vista de canto devolveria kS = 1 y se quedaria sin difuso.
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float rough)
{
    return F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Segunda mitad del split-sum (la integral del BRDF), en su forma analitica de
// Karis. Sustituye a la LUT 2D de 512x512 con un error del orden del 1%, y
// ahorra una imagen, un binding y un pass de precomputacion.
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

    // Profundidad en view space. Ya no la usa la sombra —la eleccion de capa
    // vive en shadow_lookup.glsl y la recalcula ahi— pero si el reparto en
    // slices de Forward+ clustered.
    float viewDepth = -(ubo.view * vec4(fragWorldPos, 1.0)).z;
    float shadow    = computeShadow(fragWorldPos, normalize(fragNormal));
    vec3  Lo        = vec3(0.0);

    // Forward+ apagado: el bucle de siempre sobre las MAX_LIGHTS del UBO, sin
    // tocar un solo buffer del set 2. Va copiado y no factorizado con el de
    // abajo a proposito: son las mismas operaciones en el mismo orden, y la
    // unica diferencia es de donde sale cada luz.
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

        float s = (i == 0) ? shadow : 1.0;

        Lo += att * s * (kD * albedo / PI + D * G * F / (4.0 * NdotV * NdotL + 0.0001))
              * radiance * NdotL;
    }
    }
    else
    {
        // Celda de este fragmento. gl_FragCoord va en pixeles del target, que es
        // la resolucion INTERNA — la misma con la que se dimensiono la rejilla.
        uvec2 tile = uvec2(gl_FragCoord.xy) / fp.tileSize;
        tile = min(tile, uvec2(fp.gridX - 1u, fp.gridY - 1u));

        uint cell;
        if (fp.mode == 1u)
        {
            cell = tile.y * fp.gridX + tile.x;
        }
        else
        {
            // Inverso exacto del reparto logaritmico de light_cull_clustered.comp.
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
            // Ventana por radio: fuera del radio da EXACTAMENTE 0, que es lo que
            // hace que meter luces de mas en una celda no cambie el resultado —
            // y por tanto que tiled y clustered, que culean con volumenes
            // distintos, den la misma imagen.
            float w   = clamp(1.0 - (dist * dist) / (lr * lr), 0.0, 1.0);
            float att = w * w;

            vec3  L = toL / max(dist, 1e-4);

            // Directional: sin posicion ni atenuacion. Va aparte del radio de
            // arriba porque el binning la mete en TODAS las celdas.
            if (lt == 2)
            {
                L   = normalize(-fpLights[li].direction.xyz);
                att = 1.0;
            }
            else if (lt == 1)
            {
                // Spot: mismo cono suave que lightSample().
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

            // La luz 0 sigue siendo la que proyecta las cascadas, igual que en la
            // rama de arriba: aqui se compara el indice GLOBAL, no el de la celda.
            float s = (li == 0u) ? shadow : 1.0;

            Lo += s * (kD * albedo / PI + D * G * F / (4.0 * NdotV * NdotL + 0.0001))
                  * radiance * NdotL * att;
        }
    }

    // ── Ambiente: IBL ───────────────────────────────────────────────────────
    float NdotVamb = max(dot(N, V), 0.0);
    vec3  Famb     = fresnelSchlickRoughness(NdotVamb, F0, rough);
    // Un metal no tiene difuso, y lo que refleja de especular no lo transmite.
    vec3  kDamb    = (1.0 - Famb) * (1.0 - metal);

    // El cubemap guarda ya E/PI, asi que el 1/PI del BRDF lambertiano no se
    // vuelve a aplicar aqui.
    vec3 diffuseIBL = texture(irradianceMap, N).rgb * albedo;

    // La rugosidad elige el mip: el ultimo es el lobulo mas ancho.
    vec3 R           = reflect(-V, N);
    vec3 prefiltered = textureLod(prefilterMap, R, rough * float(IBL_PREFILTER_MIPS - 1)).rgb;
    vec3 specularIBL = prefiltered * envBRDFApprox(F0, rough, NdotVamb);

    // El multiplicador escala difuso y especular por igual: sube o baja el peso
    // del entorno sin cambiar su color ni el balance entre los dos terminos.
    // El SSAO entra AQUI y no sobre el color final: es oclusion del entorno, y
    // aplicarselo tambien a la luz directa apagaria sombras que ya calcula el
    // shadow map. Se muestrea por coordenada de pantalla; el mapa es del tamano
    // exacto del framebuffer, asi que la division es 1:1 y no hace falta llevar
    // la resolucion en ningun sitio.
    float ssao   = texture(ssaoMap, gl_FragCoord.xy / vec2(textureSize(ssaoMap, 0))).r;
    vec3 ambient = (kDamb * diffuseIBL + specularIBL) * ao * ssao * ubo.ambientIntensity;
    vec3 color   = ambient + Lo;

    // Sin tonemapear: el attachment de este pass es R16G16B16A16_SFLOAT y lo
    // consume la cadena de bloom, que necesita el rango alto intacto. El ACES +
    // gamma que habia aqui vive ahora en shaders/bloom_composite.frag, que es el
    // unico sitio del motor donde HDR pasa a LDR.
    // El alfa lleva la fuerza de SSR del objeto, no opacidad: ssr.comp lo lee
    // como mascara por pixel. Antes de esta feature valia 1.0 y no lo leia nadie
    // (bloom_composite.frag y bloom_down.comp solo usan .rgb), asi que con el SSR
    // desactivado la imagen sale exactamente igual.
    outColor = vec4(color, push.flags.y);
}
