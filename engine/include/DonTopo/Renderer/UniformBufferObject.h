#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace DonTopo 
{
    // How many lights fit in the UBO, and therefore how many the scene lights:
    // Scene::collectLights discards those beyond this.
    //
    // It was 16 and the Forward+ culling distributed up to 256 that never reached it
    // (H1). It is raised to 64 now and not before because the audit
    // conditioned the spending on the clipping happening in a REAL scene, and it does:
    // lightTest.json of the test project has 18 lights (1 directional and 17
    // point), that is, two were being thrown away.
    //
    // The cost is UBO memory and nothing else: 2 KB -> 5 KB, with the minimum that
    // the spec guarantees for maxUniformBufferRange at 16 KB. And the shader
    // loop goes up to numLights, not up to this limit, so raising it costs
    // not a single GPU iteration.
    //
    // Changing this number SHIFTS everything that comes after the array in the UBO
    // block: the #define of the three shaders that declare it must be touched
    // (pbr.frag, triangle.frag, fog.comp) and the offsets of the static_assert of
    // D3D12Renderer.cpp IN THE SAME COMMIT, or the shader reads the block
    // shifted, which gives no error in any validation layer, only odd pixels. The HLSL
    // is not touched: the build generates it with spirv-cross from these same GLSL.
    constexpr int MAX_LIGHTS = 64;

    // Cascaded shadow maps: no. of cascades of the key light's shadow map. It has
    // to be worth the same here, in the array of the shaders' UBO block and in
    // the layers of the shadow map's texture array: if it gets out of sync, std140
    // silently shifts everything that comes after lightSpaceMatrix.
    constexpr int SHADOW_CASCADES = 4;

    // Matrices to reserve in a frame's instance SSBO, in the
    // worst case: each object visible in the FOUR cascades of the shadow map, plus
    // the scene pass, plus the depth pre-pass that feeds the SSAO and
    // the fog. Hence the (SHADOW_CASCADES + 2).
    //
    // Characters count the same as static ones and that was the bug (H23):
    // the count came only from the static ones, so a scene of pure
    // characters reserved ZERO and the shadow pass left through a silent
    // `break`. No error, no warning, and the shadow was simply not there.
    //
    // It saturates instead of wrapping: an absurd count has to ask for TOO MUCH,
    // which fails on allocation and is seen, not too little, which goes back to the silent failure.
    constexpr uint32_t shadowInstanceCapacity(uint32_t staticCount, uint32_t skinnedCount)
    {
        constexpr uint64_t kPasses = static_cast<uint64_t>(SHADOW_CASCADES) + 2;
        const uint64_t total = (static_cast<uint64_t>(staticCount) +
                                static_cast<uint64_t>(skinnedCount)) * kPasses;
        return total > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(total);
    }

    // LAYERS (not lights) available for the shadows that are not the key.
    //
    // It is counted in layers because each type consumes its own: a narrow spot one,
    // and a point light (or a spot so wide that a single face does not suit it)
    // the six of a cubemap. With six, ONE point light or six spots fit. A secondary
    // DIRECTIONAL one does not go in: it would need its four cascades to avoid
    // looking worse than with no shadow, and then it costs the same as the key.
    //
    // The brake is MEMORY. These layers live in the SAME texture array as
    // the key's (an array has a single resolution), so at 2048 each
    // one is 16 MB: six are 96. Giving them their own smaller array (512 = 1 MB
    // per layer) would allow raising them a lot, but requires a new binding and sampler
    // in the six shaders that declare the block, and rebuilding the
    // descriptor sets of meshes, materials and fog. It was left out on purpose:
    // it is not needed for a secondary point light to cast a shadow.
    constexpr int SHADOW_EXTRA_LAYERS = 6;

    // Shadow matrix slots in the UBO, and shadow map layers.
    //
    // The first six belong to the KEY light, which is the only one that can use more than
    // one: 4 if it is directional (one per cascade), 6 if it is a point light or a very
    // wide spot (one per cubemap face), 1 if it is a normal spot. They never coexist
    // because there is only one key light and it has only one type.
    //
    // The SHADOW_EXTRA_LAYERS behind them belong to the secondary ones: one layer per
    // narrow spot, six per point light or very wide spot.
    //
    // It has to be worth the same here and in the SIX shaders that declare the
    // UBO block: if it gets out of sync, std140 silently shifts everything that comes
    // after and no validation layer gives it away.
    constexpr int SHADOW_KEY_MATRICES = 6;
    constexpr int SHADOW_MATRICES     = SHADOW_KEY_MATRICES + SHADOW_EXTRA_LAYERS;

    // Light type. It goes in direction.w (float) and not in a separate int: std140
    // would align the int to 16 bytes anyway, so using the slot the vec4 already
    // had free comes for free.
    enum class LightType : int { Point = 0, Spot = 1, Directional = 2, Area = 3 };

    struct Light
    {
        glm::vec4 position {0.0f, 0.0f, 0.0f, 0.0f};    // xyz world, w unused
        glm::vec4 color { 1.0f, 1.0f, 1.0f, 1.0f};      // rgb = color, a = intensity
        // xyz = normalized direction (local -Z of the GameObject), w = type
        // (0 point, 1 spot, 2 directional, 3 area).
        glm::vec4 direction {0.0f, -1.0f, 0.0f, 0.0f};
        // x = range, y = cos(inner angle), z = cos(outer angle),
        // w = area width.
        glm::vec4 params {10.0f, 0.9f, 0.7f, 1.0f};
    };

    // Point that a light without its own direction aims at: the center of the SCENE,
    // as the mean of the origins of what is drawn.
    //
    // Stable on purpose. The previous version aimed at the center of the camera's
    // frustum, which sounds better (the shadow falls where you are looking) but ties the
    // light direction to the camera: turning in place turned the shadow, and that
    // looks MUCH worse than the problem it fixed. With the scene center
    // the shadow only moves when the light moves, which is what one expects.
    //
    // The origins and not the bounding boxes: this only chooses the direction of
    // an approximation, and an exact centroid would not improve it by anything
    // noticeable. It is the same criterion D3D12's camera range already used
    // with characters.
    struct SceneCenter
    {
        glm::vec3 suma{0.0f};
        int       n = 0;

        void add(const glm::vec3& origen) { suma += origen; ++n; }
        // The fourth column of a world transform, which is the case for both
        // backends.
        void add(const glm::mat4& transform) { add(glm::vec3(transform[3])); }

        // false = empty scene. The caller decides: the cascades skip the
        // pass (there is nothing to shadow) and the fog keeps its neutral
        // direction.
        bool get(glm::vec3& out) const
        {
            if (n == 0) return false;
            out = suma / static_cast<float>(n);
            return true;
        }
    };

    // Near plane of a spot's perspective shadow map. Constant and not
    // adjustable: the only thing noticed when raising it is that a caster right next to the
    // bulb stops casting, and lowering it further spreads the z precision
    // even worse. It lives here because both backends have to use EXACTLY the
    // same one, or the shadow bias works out in one and not in the other.
    constexpr float SPOT_SHADOW_NEAR = 0.05f;

    // Shadow matrix of a SPOT: PERSPECTIVE projection from its position,
    // with the FOV of its cone. It is what makes its shadow DIVERGE (grow when
    // moving away from the light) instead of keeping its size like the cascade
    // approximation, which is for directional light.
    //
    // It goes in lightSpaceMatrix[0] and is recorded in layer 0 of the same shadow map as
    // always: the cascades and this never coexist, because there is only one key
    // light and it has only one type. That is why neither a new resource nor a new
    // binding is needed.
    //
    // flipY: whoever absorbs the backend's Y convention. Both leave the map
    // in the SAME orientation, which is what the shared sampling takes for
    // granted, but by different paths:
    //
    //   Vulkan  -> flipY = true.  The matrix does it, like the orthographic one of
    //                             the cascades (ShadowPass.cpp).
    //   D3D12   -> flipY = false. The negative-height viewport of the
    //                             shadow pass does it (shadowViewport.Height < 0).
    //
    // Setting BOTH or NEITHER gives no error in any validation layer and the
    // symptom is not a displaced shadow, which would be seen right away: it is a
    // flicker. With the map mirrored in v the depth comparison lands on a
    // texel that has nothing to do with it, so shadow and light come out almost at random
    // over the surface and the TAA jitter stirs them on every frame. It is
    // easily confused with lack of bias, and raising the bias does not touch it.
    //
    // false = the light has no usable direction and the caller must skip
    // the pass.
    // FOV that a spot's shadow map would need, in radians. Above
    // 90 degrees a single face stops being the right technique: the footprint of a
    // texel is 2*d*tan(FOV/2)/resolution, and tan() blows up near 180: at 150
    // degrees it is 3.7 times worse than at 90, and at 175 it is 23 times. Hence the threshold
    // of spotNecesitaCubemap.
    inline float spotShadowFov(const glm::vec4& params)
    {
        // params.z = COSINE of the cone's outer angle, that is, the half-angle.
        // The map's FOV is the full angle (double) and also with margin:
        // without it, the cone's edge falls right on the map's edge and the PCF
        // taps go out one side.
        const float cosOuter = glm::clamp(params.z, -0.9999f, 0.9999f);
        return glm::clamp(2.0f * std::acos(cosOuter) * 1.15f,
                          glm::radians(5.0f), glm::radians(175.0f));
    }

    // A spot so wide that it no longer fits well in one face: it is given the six-face
    // cubemap, like a point light. Its cone still clips the LIGHT in the
    // shader; the only thing that changes is that the shadow map covers more than is
    // needed, in exchange for no face going past 90 degrees.
    inline bool spotNecesitaCubemap(const glm::vec4& params)
    {
        return spotShadowFov(params) > glm::radians(90.0f);
    }

    inline bool spotShadowMatrix(const glm::vec4& position, const glm::vec4& direction,
                                 const glm::vec4& params, bool flipY, glm::mat4& out)
    {
        const glm::vec3 d(direction);
        const float     l = glm::length(d);
        if (l < 1e-6f) return false;
        const glm::vec3 dir = d / l;
        const glm::vec3 pos(position);

        const float fov = spotShadowFov(params);

        // params.x = reach of the light. Beyond it, it does not illuminate, so there is no
        // shadow of it to record either, and bounding the far there is what gives depth precision
        // to the piece that is used.
        const float lejos = (std::max)(params.x, SPOT_SHADOW_NEAR * 2.0f);

        const glm::vec3 up = std::abs(dir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                     : glm::vec3(0.0f, 1.0f, 0.0f);

        // *RH_ZO and not plain glm::perspective: the plain one gives z in [-1,1] and Vulkan
        // clips the near half.
        glm::mat4 proj = glm::perspectiveRH_ZO(fov, 1.0f, SPOT_SHADOW_NEAR, lejos);
        // On the PROJECTION and before multiplying. Doing it afterwards, on
        // out[1][1], would be something else: in the product row 1 already has the
        // view mixed in, and negating a single element of that row is not equivalent to negating it
        // entirely.
        if (flipY) proj[1][1] *= -1.0f;

        out = proj * glm::lookAt(pos, pos + dir, up);
        return true;
    }

    // The SIX matrices of the shadow cubemap of a POINT light: one per
    // face, 90-degree perspective projection from its position. It is what
    // makes its shadow diverge in ALL directions, which is what a
    // bulb does; the cascades only knew how to project in parallel along one.
    //
    // Face order: 0 = +X, 1 = -X, 2 = +Y, 3 = -Y, 4 = +Z, 5 = -Z. The shader
    // chooses the face by the LARGEST axis of (fragment - light) and then projects with
    // THIS SAME matrix, not with a formula of its own. It is deliberate: deriving the UV by
    // hand forces two cubemap conventions to match, and in this engine
    // that already went wrong once. This way the only thing to get right is WHICH of the
    // six, and getting it wrong shows up as a hard cut, not as subtle garbage.
    //
    // For that same reason each face's "up" vector does not matter as long as it is the same
    // here and when recording: it only rotates the face around itself, and the matrix that
    // undoes that rotation is the one used for sampling.
    //
    // Exactly 90 degrees and aspect 1: it is the only thing that makes the six faces
    // cover the whole sphere with no gap or overlap.
    //
    // flipY: same as in spotShadowMatrix: Vulkan true, D3D12 false.
    // false = the light has no usable reach.
    inline bool pointShadowMatrices(const glm::vec4& position, const glm::vec4& params,
                                    bool flipY, glm::mat4* out /*[6]*/)
    {
        const glm::vec3 pos(position);
        const float     lejos = (std::max)(params.x, SPOT_SHADOW_NEAR * 2.0f);
        if (!std::isfinite(lejos) || lejos <= SPOT_SHADOW_NEAR) return false;

        glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f,
                                               SPOT_SHADOW_NEAR, lejos);
        if (flipY) proj[1][1] *= -1.0f;

        static const glm::vec3 kDir[6] = {
            { 1.0f,  0.0f,  0.0f}, {-1.0f,  0.0f,  0.0f},
            { 0.0f,  1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f},
            { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f, -1.0f},
        };
        // For +Y and -Y the up cannot be (0,1,0) or lookAt degenerates.
        static const glm::vec3 kUp[6] = {
            {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f},
            {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        };

        for (int f = 0; f < 6; f++)
            out[f] = proj * glm::lookAt(pos, pos + kDir[f], kUp[f]);
        return true;
    }

    // Margin behind each cascade's volume, in the light direction.
    // Without it, a tall object that falls outside the camera frustum but whose
    // shadow does fall inside would not be drawn in the shadow map.
    constexpr float SHADOW_CASTER_MARGIN = 200.0f;

    // Cascade split of a DIRECTIONAL light: the depth cuts and the
    // orthographic matrix of each one.
    //
    // It was written TWICE, once per backend, with the same math and the
    // same constants. Nobody detected a divergence: a tweak in one copy
    // left the shadows different in the other backend and it only showed when placing
    // the two scenes side by side (H3).
    //
    //   view/proj      camera of the frame. proj may carry the Vulkan Y-flip
    //                  inside or not: here it only serves to unproject the
    //                  frustum, and the NDC cube is symmetric.
    //   lightDir       direction already resolved by keyLightDirection.
    //   maxDistance    shadow reach (RendererState::shadowDistance).
    //   lambda         blend between logarithmic and uniform split.
    //   shadowMapSize  side of the map, for the texel snap.
    //   flipY          same as in spotShadowMatrix: Vulkan true, D3D12 false.
    //
    // false = the projection is degenerate or the reach does not even get to the near, and the
    // caller must leave the matrices as they were.
    inline bool cascadeShadowMatrices(const glm::mat4& view, const glm::mat4& proj,
                                      const glm::vec3& lightDir,
                                      float maxDistance, float lambda,
                                      uint32_t shadowMapSize, bool flipY,
                                      glm::mat4* outMatrices /*[SHADOW_CASCADES]*/,
                                      glm::vec4& outSplits)
    {
        if (shadowMapSize == 0) return false;

        // Corners of the frustum, unprojecting the NDC cube. z goes from 0 to 1 and not
        // from -1 to 1 because that is the range that Vulkan and D3D12 clip: what is
        // really drawn is always between those two planes.
        const glm::mat4 invViewProj = glm::inverse(proj * view);
        glm::vec3       cornerNear[4], cornerFar[4];
        const float     ndcX[4] = { -1.0f,  1.0f,  1.0f, -1.0f };
        const float     ndcY[4] = { -1.0f, -1.0f,  1.0f,  1.0f };
        for (int i = 0; i < 4; i++)
        {
            const glm::vec4 pn = invViewProj * glm::vec4(ndcX[i], ndcY[i], 0.0f, 1.0f);
            const glm::vec4 pf = invViewProj * glm::vec4(ndcX[i], ndcY[i], 1.0f, 1.0f);
            if (std::abs(pn.w) < 1e-8f || std::abs(pf.w) < 1e-8f) return false;
            cornerNear[i] = glm::vec3(pn) / pn.w;
            cornerFar[i]  = glm::vec3(pf) / pf.w;
        }

        // REAL near/far: the view space depth of those two planes. They are not
        // taken from the proj coefficients on purpose: the editor builds
        // its projection with glm::perspective (z in [-1,1]) and the CameraComponent
        // with *RH_ZO, so the same coefficients mean different things
        // and the formula would have to know which one is active. The planes z=0 and z=1,
        // on the other hand, are the same in both cases.
        const float camNear = -(view * glm::vec4(cornerNear[0], 1.0f)).z;
        const float camFar  = -(view * glm::vec4(cornerFar[0],  1.0f)).z;
        if (!std::isfinite(camNear) || !std::isfinite(camFar) ||
            camNear <= 0.0f || camFar <= camNear)
        {
            return false;
        }

        // The corners are already set with the REAL far (it is the one that defines the
        // frustum rays); the cascade split uses the clipped far.
        const float shadowFar = (std::min)(camFar, maxDistance);
        if (shadowFar <= camNear) return false;

        const glm::vec3 up = std::abs(lightDir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                          : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::mat4 lightRot    = glm::lookAt(glm::vec3(0.0f), lightDir, up);
        const glm::mat4 invLightRot = glm::inverse(lightRot);

        float prevDist = camNear;
        for (int c = 0; c < SHADOW_CASCADES; c++)
        {
            const float p        = (float)(c + 1) / (float)SHADOW_CASCADES;
            const float logSplit = camNear * std::pow(shadowFar / camNear, p);
            const float uniSplit = camNear + (shadowFar - camNear) * p;
            const float dist     = lambda * logSplit + (1.0f - lambda) * uniSplit;
            outSplits[c]         = dist;

            // Interpolating between the near and far corners is exact: the
            // view space depth varies linearly along that
            // segment. The factors are computed against the REAL far because it is
            // the one that places cornerFar.
            const float tNear = (prevDist - camNear) / (camFar - camNear);
            const float tFar  = (dist     - camNear) / (camFar - camNear);

            glm::vec3 corners[8];
            for (int i = 0; i < 4; i++)
            {
                const glm::vec3 ray = cornerFar[i] - cornerNear[i];
                corners[i]     = cornerNear[i] + ray * tNear;
                corners[i + 4] = cornerNear[i] + ray * tFar;
            }

            // Bounding sphere and not AABB: the radius does not depend on where
            // the camera looks, so turning in place does not change the size of the
            // volume and the shadows do not pulse.
            glm::vec3 center(0.0f);
            for (const glm::vec3& v : corners) center += v;
            center /= 8.0f;
            float radius = 0.0f;
            for (const glm::vec3& v : corners)
                radius = (std::max)(radius, glm::length(v - center));
            // Quantizing the radius keeps a minimal camera change from moving
            // the volume's edge and with it all the texels.
            radius = std::ceil(radius * 16.0f) / 16.0f;
            if (radius < 1e-4f) radius = 1e-4f;

            // Snap of the center to shadow map texels, in light space.
            // Without this, advancing the camera drags the volume continuously
            // and the shadow edges boil.
            const float unitsPerTexel = (2.0f * radius) / (float)shadowMapSize;
            glm::vec3   centerLS      = glm::vec3(lightRot * glm::vec4(center, 1.0f));
            centerLS.x = std::floor(centerLS.x / unitsPerTexel) * unitsPerTexel;
            centerLS.y = std::floor(centerLS.y / unitsPerTexel) * unitsPerTexel;
            center     = glm::vec3(invLightRot * glm::vec4(centerLS, 1.0f));

            const glm::mat4 lightView =
                glm::lookAt(center - lightDir * (radius + SHADOW_CASTER_MARGIN), center, up);
            glm::mat4 lightProj = glm::orthoRH_ZO(-radius, radius, -radius, radius,
                                                  0.0f, 2.0f * radius + SHADOW_CASTER_MARGIN);
            // On the PROJECTION and before multiplying, same as in
            // spotShadowMatrix: in the product row 1 already has the view mixed in
            // and negating a single element is not equivalent to negating it entirely.
            if (flipY) lightProj[1][1] *= -1.0f;

            outMatrices[c] = lightProj * lightView;
            prevDist       = dist;
        }
        return true;
    }

    // Distributes the shadow slots among the lights that are NOT the key.
    //
    // It walks lights 1..n-1 in scene order and gives one slot to each SPOT
    // until SHADOW_EXTRA_LAYERS runs out. In scene order and not by brightness or
    // closeness on purpose: if the criterion depended on the camera, a light
    // would gain and lose its shadow as you move and that flickers.
    //
    // ranuraDeLuz[i] = matrix/layer index of light i, or -1 if it does not cast.
    // Light 0 always comes out -1: its matrices are set by computeCascades and they occupy
    // the first SHADOW_KEY_MATRICES slots.
    //
    // Returns how many slots were occupied.
    //
    // It lives here and not in each backend because the split has to be IDENTICAL in
    // both: the slot decides which layer is recorded and with which matrix the
    // shader samples, so a different split per backend would be the same scene with
    // shadows on different lights.
    // capasDeLuz (optional) receives HOW MANY layers each light took: 1 one face,
    // SHADOW_KEY_MATRICES a cubemap, 0 if it does not cast. It comes out of here and is not
    // recomputed in the backend nor in the shader on purpose: it is the same criterion
    // that decides the reservation, and answering it differently elsewhere would record fewer
    // faces than reserved. It is exactly the failure of H65.
    template <typename LuzT, typename TipoDeLuz, typename ParamsDeLuz>
    inline int repartirSombrasExtra(const LuzT* luces, int n,
                                    TipoDeLuz tipoDe, ParamsDeLuz paramsDe,
                                    int* ranuraDeLuz /*[n]*/,
                                    int* capasDeLuz = nullptr /*[n]*/)
    {
        for (int i = 0; i < n; i++) ranuraDeLuz[i] = -1;
        if (capasDeLuz) for (int i = 0; i < n; i++) capasDeLuz[i] = 0;

        int usadas = 0;
        for (int i = 1; i < n && usadas < SHADOW_EXTRA_LAYERS; i++)
        {
            const int tipo = tipoDe(luces[i]);

            // How many layers THIS light needs. A point light spreads its reach
            // over the six faces of a cubemap; a narrow spot fits in one
            // face; a very wide spot also needs the cubemap, for the same
            // reason as the key (the footprint of a texel goes with tan(FOV/2) and near
            // 180 degrees it blows up).
            //
            // A secondary DIRECTIONAL one is left out: it would need its four
            // cascades to avoid looking worse than with no shadow, and then it costs the
            // same as the key. It still illuminates.
            int necesita = 0;
            if (tipo == static_cast<int>(LightType::Point))
                necesita = SHADOW_KEY_MATRICES;                     // six faces
            else if (tipo == static_cast<int>(LightType::Spot))
                necesita = spotNecesitaCubemap(paramsDe(luces[i])) ? SHADOW_KEY_MATRICES : 1;
            else
                continue;

            // It goes in COMPLETE or it does not go in. Reserving three faces of six would leave
            // the shader sampling another light's layers when choosing one of
            // the missing ones, and no validation layer warns about that: the layer
            // exists and has content, it just is not its own.
            if (usadas + necesita > SHADOW_EXTRA_LAYERS) continue;

            ranuraDeLuz[i] = SHADOW_KEY_MATRICES + usadas;
            if (capasDeLuz) capasDeLuz[i] = necesita;
            usadas += necesita;
        }
        return usadas;
    }

    // Direction in which the key light "falls": the one that builds the shadow map and the
    // one used by the fog's in-scattering. A SINGLE place on purpose: when
    // each consumer derived it on its own, changing the criterion in the
    // cascades and not in the fog left the scattering pointing one way and the
    // shadow map built toward another.
    //
    //  - Directional and spot: their OWN direction, which is the local -Z of the
    //    GameObject. A spot has a cone, so it has a real direction;
    //    before, the point light approximation was applied to it and turning its
    //    gizmo did not move its shadow.
    //  - Point: it has no direction, so it is aimed from the light to the center of the
    //    scene (aim). It is all that cascaded shadows can give, which
    //    are for directional light: the projection is still parallel and the size
    //    of the shadow does not change with distance. The correct shadow needs a
    //    cubemap (see P21 in docs/renderer-audit.md).
    //
    // aim comes from SceneCenter. Passing it the world origin reproduces the
    // previous behavior, which only worked out with the scene centered there.
    //
    // false = there is no usable direction (light right at the aim point, or
    // null direction) and the caller must skip the pass instead of dividing by
    // zero.
    inline bool keyLightDirection(const glm::vec4& position, const glm::vec4& direction,
                                  const glm::vec3& aim, glm::vec3& out)
    {
        // The type goes in direction.w, with the same convention pbr.frag uses:
        // int(w + 0.5).
        const int tipo = static_cast<int>(direction.w + 0.5f);

        if (tipo == static_cast<int>(LightType::Directional) ||
            tipo == static_cast<int>(LightType::Spot))
        {
            const glm::vec3 d(direction);
            const float     l = glm::length(d);
            if (l < 1e-6f) return false;
            out = d / l;
            return true;
        }

        const glm::vec3 haciaElCentro = aim - glm::vec3(position);
        const float     l             = glm::length(haciaElCentro);
        if (l < 1e-6f) return false;
        out = haciaElCentro / l;
        return true;
    }

    struct UniformBufferObject
    {
        glm::mat4   view;
        glm::mat4   proj;
        glm::mat4   lightSpaceMatrix[SHADOW_MATRICES];
        // Distance (view space, positive) that each cascade reaches. The
        // last one is the total reach of the shadows: beyond it, the fragment
        // shader returns "no shadow" instead of sampling.
        glm::vec4   cascadeSplits{0.0f};
        Light       lights[MAX_LIGHTS];
        glm::vec4   viewPos;
        int         numLights = 0;
        // Global ambient multiplier (IBL). It takes the FIRST slot of the
        // padding that already existed after numLights, so neither sizeof(UBO) nor the
        // offset of any previous member changes: only pbr.frag declares this
        // field, and the other 4 shaders that share the block keep seeing
        // exactly the same std140 layout as before.
        float       ambientIntensity = 1.0f;
        float       _pad[2]{};              // std140: align to 16 bytes after the int
    };

    /*
        (Light takes 4×vec4=64 bytes, already aligned to 16.
        The int numLights after the array needs 12 bytes of padding
        so that the next member (if there were one) respects std140;
        here it is the last field so the padding only ensures sizeof(UBO)
        is a multiple of 16, which mat4+mat4+4*32+16+4 = ... already satisfies)
    */
}