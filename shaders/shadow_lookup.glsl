// Sampling coordinate of the key light's shadow map.
//
// It is included by pbr.frag and fog.comp, which sample THE SAME map. Before, each had
// its own copy of the cascade selection and of the reprojection; changing one and
// not the other left the fog's in-scattering pointing to one side and the shadow
// map built towards another (H65), and no validation layer reports that failure.
// That is why what has to be identical lives here.
//
// What does NOT live here is the filtering: the scene does 3x3 PCF and the fog a single
// tap, because the fog already averages along the march and multiplying its taps by
// nine is paid at every step of every ray. That difference is
// deliberate; the layer choice and the reprojection are not.
//
// TEXTUAL INCLUSION: it goes AFTER declaring `ubo` (with view, lightSpaceMatrix[]
// and cascadeSplits) and the `shadowMap` sampler, because these functions use them by
// name. And after #define SHADOW_CASCADES.

// Type of the key light, with the same convention as the rest of the engine:
// direction.w carries the type and is read with int(w + 0.5). ONLY light 0 casts
// shadow.
//   0 point, 1 spot, 2 directional, 3 area.
int dtKeyLightType()
{
    return int(ubo.lights[0].direction.w + 0.5);
}

// Cascade that a fragment falls into by its view space depth. The
// cuts already come sorted; the last one is the total reach of the shadows.
// -1 = beyond that reach, there is no map to sample.
int dtSelectCascade(float viewDepth)
{
    for (int i = 0; i < SHADOW_CASCADES; i++)
        if (viewDepth <= ubo.cascadeSplits[i]) return i;
    return -1;
}

// Where to sample the shadow map for a world point.
//
//   uvz   = (u, v, reference depth) already in [0,1]
//   layer = array layer
//
// false = this point is not covered by the map and the caller must treat it as
// LIT (1.0). It happens beyond the reach of the cascades, and outside the cone
// of a spot light.
//
// Two paths, depending on the key light's type:
//
//  - Directional: CASCADED shadows. Orthographic projection, that is, parallel
//    shadow, which is right for a light that is at infinity. The
//    cascade is chosen by camera depth.
//
//  - Spot: ONE face in PERSPECTIVE from the light's position, in layer 0.
//    Its matrix goes in lightSpaceMatrix[0] (the cascades and this never
//    coexist, because there is only one key light and it has only one type), and the
//    perspective is what makes its shadow DIVERGE: it grows as it moves away from the light, instead
//    of keeping its size as the cascade approximation did.
//
//  - Point, and spot wider than 90 degrees: six-face CUBEMAP, one per half-axis,
//    in layers 0..5. Each one is a 90 degree perspective from the light,
//    so the shadow diverges in all directions and no face spreads
//    its texels over more than one octant. It is the only path that uses more than
//    four lightSpaceMatrix slots, and that is why that array has
//    SHADOW_MATRICES = 6.

// WORLD-SPACE bias for the two PERSPECTIVE paths (spot and point).
// The rasterizer's one is tuned for the cascades' orthographic projection, where the
// NDC depth is linear in the real distance; in perspective it is not, and
// correcting it there would force a couple of extra PSOs in EACH backend because the
// bias is pipeline state in both.
//
// Two terms, which attack different things:
//
//  - NORMAL-OFFSET proportional to the distance to the light. In perspective the
//    footprint of a texel GROWS with that distance, and so does the quantization error;
//    a fixed offset that works near the bulb falls short far away.
//    Moving away along the normal is what takes the surface out of its own shadow,
//    because the error is in the surface's plane.
//
//  - Push TOWARDS THE LIGHT, small and constant, for the surface seen almost edge-on
//    from the light, where the normal barely separates in depth.
//
// worldNormal == vec3(0) -> point in the AIR (the fog march): it cannot
// self-shadow, so only the push is applied.
vec3 dtBiasHaciaLaLuz(int luz, vec3 worldPos, vec3 normalMundo)
{
    vec3  aLaLuz  = ubo.lights[luz].position.xyz - worldPos;
    float distLuz = length(aLaLuz);
    if (distLuz <= 1e-5) return worldPos;
    return worldPos + (aLaLuz / distLuz) * 0.02 + normalMundo * (distLuz * 0.004);
}

// worldNormal = GEOMETRIC normal of the surface, for the bias of the
// perspective paths. A point that is not on any surface (the fog
// march) passes vec3(0.0).
// A single perspective face, in the given slot. It is used by the key spot (slot
// 0) and by each secondary spot (slots SHADOW_KEY_MATRICES onwards), so that
// the cone clipping and the bias live in a single place.
bool dtCaraPerspectiva(int luz, int ranura, vec3 worldPos, vec3 normalMundo,
                       out vec3 uvz, out float layer)
{
    worldPos = dtBiasHaciaLaLuz(luz, worldPos, normalMundo);

    vec4 ls = ubo.lightSpaceMatrix[ranura] * vec4(worldPos, 1.0);
    // Behind the light: w <= 0 makes the division return the mirrored point,
    // which would fall inside the map and paint a ghost shadow on the other side.
    if (ls.w <= 0.0) return false;

    vec3 p = ls.xyz / ls.w;
    p.xy   = p.xy * 0.5 + 0.5;
    // Outside the spot's frustum nothing is recorded. The clipping in xy is
    // mandatory here and not in the cascades: a cascade's volume is fitted
    // to what is seen, while a spot's cone leaves out almost the whole
    // scene, and without this the sampler stretches the map's edge over the rest
    // of the world.
    if (p.z < 0.0 || p.z > 1.0) return false;
    if (any(lessThan(p.xy, vec2(0.0))) || any(greaterThan(p.xy, vec2(1.0)))) return false;

    uvz   = p;
    layer = float(ranura);
    return true;
}

// Picks a cubemap face by the MAJOR axis of (fragment - light): it is the one that
// looks at that half-space, and its 90 degree frustum contains the point.
//
// It only picks WHICH one. The UV and the depth come out afterwards from that face's
// matrix, the same one it was recorded with. Deriving them by hand would require two
// cubemap conventions to match, and that already went wrong here once.
int dtCaraDelCubemap(vec3 worldPos, vec3 posLuz)
{
    vec3 L = worldPos - posLuz;
    vec3 a = abs(L);
    if (a.x >= a.y && a.x >= a.z) return L.x > 0.0 ? 0 : 1;
    if (a.y >= a.z)               return L.y > 0.0 ? 2 : 3;
    return L.z > 0.0 ? 4 : 5;
}

// Perspective sampling from an already chosen slot, without clipping in xy: it is
// used by the faces of a cubemap, where going out through one side is normal (the point
// belongs to the neighboring face) and clipping it would leave a hard cut on the diagonal.
bool dtCaraDeCubemap(int luz, int ranura, vec3 worldPos, vec3 normalMundo,
                     out vec3 uvz, out float layer)
{
    worldPos = dtBiasHaciaLaLuz(luz, worldPos, normalMundo);

    vec4 ls = ubo.lightSpaceMatrix[ranura] * vec4(worldPos, 1.0);
    if (ls.w <= 0.0) return false;

    vec3 p = ls.xyz / ls.w;
    p.xy   = p.xy * 0.5 + 0.5;
    // Outside the light's reach nothing is recorded, and beyond the far plane
    // it does not light either: "no shadow" is the right answer.
    if (p.z < 0.0 || p.z > 1.0) return false;

    uvz   = p;
    layer = float(ranura);
    return true;
}

// Shadow of a light that is NOT the key, in the slot the allocation gave it.
//
// position.w encodes both things in a single field, because there is no other free
// one in the block: |w| - 1 is the slot, and the SIGN says which path it was
// recorded through: positive a single face (narrow spot), negative the six of a
// cubemap (point light, or a spot so wide that a single face fits it badly). The
// renderer sets it from what the pass DID; the shader does not deduce the path
// from the light type, which would be a second copy of the criterion.
//
// false = this light has no shadow here and the caller treats it as LIT.
bool dtShadowCoordExtra(int luz, vec3 worldPos, vec3 normalMundo,
                        out vec3 uvz, out float layer)
{
    uvz   = vec3(0.0);
    layer = 0.0;

    float codigo = ubo.lights[luz].position.w;
    if (abs(codigo) < 0.5) return false;          // does not cast

    int  ranura  = int(abs(codigo) + 0.5) - 1;
    bool cubemap = codigo < 0.0;
    if (ranura < SHADOW_KEY_MATRICES || ranura >= SHADOW_MATRICES) return false;

    if (!cubemap)
        return dtCaraPerspectiva(luz, ranura, worldPos, normalMundo, uvz, layer);

    // The six faces are in CONSECUTIVE slots starting from the one the allocation gave,
    // in the same order pointShadowMatrices recorded them.
    int cara = dtCaraDelCubemap(dtBiasHaciaLaLuz(luz, worldPos, normalMundo),
                                ubo.lights[luz].position.xyz);
    if (ranura + cara >= SHADOW_MATRICES) return false;
    return dtCaraDeCubemap(luz, ranura + cara, worldPos, normalMundo, uvz, layer);
}

bool dtShadowCoord(vec3 worldPos, vec3 normalMundo, out vec3 uvz, out float layer)
{
    uvz   = vec3(0.0);
    layer = 0.0;

    int tipo = dtKeyLightType();

    // position.w of the key light = 1 when its shadow was recorded as a CUBEMAP. The
    // renderer sets it from how many layers the pass left valid, and it is NOT
    // deduced here from the type: a point light always goes through a cubemap, but a very wide
    // SPOT does too (above a 90 degree cone a single face
    // spreads the texels over so much world that the edge comes out stepped, and it
    // gets worse with tan(FOV/2)). Recomputing that threshold here would be a second
    // copy of the criterion, which is exactly what broke H65.
    if (ubo.lights[0].position.w > 0.5)   // six-face cubemap
    {
        worldPos = dtBiasHaciaLaLuz(0, worldPos, normalMundo);

        // The face is decided by the MAJOR axis of (fragment - light): it is the one that looks at
        // that half-space, and its 90 degree frustum contains the point. Only WHICH one is
        // picked; the UV and the depth come from that face's matrix, the
        // same one it was recorded with. Deriving them by hand would require two cubemap
        // conventions to match, and that already went wrong here.
        vec3  L = worldPos - ubo.lights[0].position.xyz;
        vec3  a = abs(L);
        int   cara;
        if (a.x >= a.y && a.x >= a.z)      cara = L.x > 0.0 ? 0 : 1;
        else if (a.y >= a.z)               cara = L.y > 0.0 ? 2 : 3;
        else                               cara = L.z > 0.0 ? 4 : 5;

        vec4 ls = ubo.lightSpaceMatrix[cara] * vec4(worldPos, 1.0);
        if (ls.w <= 0.0) return false;

        vec3 p = ls.xyz / ls.w;
        p.xy   = p.xy * 0.5 + 0.5;
        // Outside the light's reach nothing is recorded: beyond the far plane of
        // the projection it does not light either, so "no shadow" is correct.
        if (p.z < 0.0 || p.z > 1.0) return false;

        uvz   = p;
        layer = float(cara);
        return true;
    }

    if (tipo == 1)   // key spot: a single face, in slot 0
    {
        // It shows more when MOVING than when still because TAA accumulates history
        // while the camera is stopped and rejects it as soon as it moves: without
        // enough bias, the acne pattern changes with the subpixel jitter and
        // TAA can no longer average it out.
        return dtCaraPerspectiva(0, 0, worldPos, normalMundo, uvz, layer);
    }

    // Directional and point: cascades.
    float viewDepth = -(ubo.view * vec4(worldPos, 1.0)).z;
    int   cascade   = dtSelectCascade(viewDepth);
    if (cascade < 0) return false;

    vec4 ls = ubo.lightSpaceMatrix[cascade] * vec4(worldPos, 1.0);
    vec3 p  = ls.xyz / ls.w;
    p.xy    = p.xy * 0.5 + 0.5;
    if (p.z > 1.0 || p.z < 0.0) return false;

    uvz   = p;
    layer = float(cascade);
    return true;
}
