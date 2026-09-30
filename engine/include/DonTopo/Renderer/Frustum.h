#pragma once
#include <glm/glm.hpp>

#include <cmath>

namespace DonTopo
{
    // Frustum culling, with nothing from any graphics API: used by the Vulkan
    // backend and the DirectX 12 one, and it will be used by the next. It lived inside
    // Renderer (Vulkan) until there was a second backend that needed it.
    namespace Culling
    {
        // Six planes in world space, with the normal pointing INTO the
        // volume: a point is visible if it lies on the positive side of all
        // six. Each plane is (nx, ny, nz, d) with the normal normalized, so
        // dot(n, p) + d is the signed distance.
        struct Frustum {
            glm::vec4 planes[6] = {};
        };

        // Extracts the planes from a viewProj matrix (Gribb-Hartmann).
        inline Frustum frustumFromViewProj(const glm::mat4& m)
        {
            // glm is column-major: m[col][row]. The rows of the matrix, which is
            // what Gribb-Hartmann needs, have to be composed by hand.
            const glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
            const glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
            const glm::vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
            const glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);

            Frustum f;
            f.planes[0] = r3 + r0;  // left
            f.planes[1] = r3 - r0;  // right
            f.planes[2] = r3 + r1;  // bottom
            f.planes[3] = r3 - r1;  // top
            // Near by the OpenGL convention (r3 + r2) and NOT by the Vulkan one
            // (plain r2), on purpose: this engine mixes the two depth ranges
            // (the editor builds its projection with glm::perspective,
            // which without GLM_FORCE_DEPTH_ZERO_TO_ONE gives z=[-1,1]; CameraComponent and
            // the light use *RH_ZO, which gives z=[0,1]). On a ZO matrix, r3+r2
            // describes a plane somewhat BEHIND the real near: it clips less
            // than it could, but never discards something that would be seen. The other way
            // around (r2 on a [-1,1] matrix) would eat the near half of the
            // scene, and the symptom would be objects disappearing as you approach.
            f.planes[4] = r3 + r2;  // near
            f.planes[5] = r3 - r2;  // far

            // Normalize: without this, dot(n,c)+d is not a distance and the projected
            // radius of the AABB would not be comparable with it.
            for (glm::vec4& p : f.planes)
            {
                const float len = glm::length(glm::vec3(p));
                if (len > 0.0f)
                    p /= len;
            }
            return f;
        }

        // AABB in the mesh's LOCAL space + its transform to world. Returns false
        // only if the box lies entirely outside some plane; it is a conservative
        // test (it may give extra true at the frustum corners, never
        // a spurious false, which would be a vanished object).
        inline bool aabbVisible(const Frustum& frustum, const glm::vec3& localMin,
                                const glm::vec3& localMax, const glm::mat4& model)
        {
            // Center + half-axes instead of the 8 corners: the per-plane test comes out
            // to two dot products instead of eight.
            const glm::vec3 localCenter = (localMin + localMax) * 0.5f;
            const glm::vec3 localExtent = (localMax - localMin) * 0.5f;

            const glm::vec3 center = glm::vec3(model * glm::vec4(localCenter, 1.0f));

            // Half-axes of the AABB that wraps the already transformed box. The
            // absolute value of the 3x3 is what turns a rotation into
            // "how much the axis-aligned box grows"; with non-uniform scaling
            // it comes out right all the same.
            const glm::mat3 rs = glm::mat3(model);
            const glm::vec3 extent(std::abs(rs[0][0]) * localExtent.x +
                                       std::abs(rs[1][0]) * localExtent.y +
                                       std::abs(rs[2][0]) * localExtent.z,
                                   std::abs(rs[0][1]) * localExtent.x +
                                       std::abs(rs[1][1]) * localExtent.y +
                                       std::abs(rs[2][1]) * localExtent.z,
                                   std::abs(rs[0][2]) * localExtent.x +
                                       std::abs(rs[1][2]) * localExtent.y +
                                       std::abs(rs[2][2]) * localExtent.z);

            for (const glm::vec4& p : frustum.planes)
            {
                const glm::vec3 n(p);
                const float     distance = glm::dot(n, center) + p.w;
                // Radius of the box projected onto the plane normal: the
                // corner that sticks out the most toward the positive side.
                const float radius = std::abs(n.x) * extent.x + std::abs(n.y) * extent.y +
                                     std::abs(n.z) * extent.z;
                if (distance + radius < 0.0f)
                    return false;  // entirely on the outside
            }
            return true;
        }
    }  // namespace Culling
}  // namespace DonTopo
