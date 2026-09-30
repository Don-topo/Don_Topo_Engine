#pragma once
// INTERNAL physics header: it includes PhysX, so it does not live in
// engine/include —dragging it into the public API would force everyone who uses a collider
// to compile against PhysX—. It is included by the .cpp files of PhysicsManager
// and of the four colliders.
#include <PxPhysicsAPI.h>

#include "DonTopo/Core/TransformDecompose.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace DonTopo
{
    // PhysX pose and scale from a worldTransform, safe against matrices
    // that cannot be decomposed.
    //
    // THE BUG THIS CLOSES. Setting Scale.Y = 0 in the inspector makes the matrix
    // singular. `glm::decompose` returns a **bool**, and for such a matrix
    // it returns `false` WITHOUT WRITING any of its outputs. Nobody looked at that
    // return value —neither here nor in the twelve copies this
    // function came from—, so what was used were the **uninitialized** local
    // variables: in Debug, the CRT 0xCDCDCDCD pattern, which as a float
    // is -1.07374e+08.
    //
    // From that single failure came three symptoms that looked like different bugs:
    //
    //  1. The pose went out with that value in the rotation. PhysX rejects it, returns
    //     null, the physxCheck of PhysicsManager throws, nobody catches it and the
    //     process DIES: exit code 3 and ZERO output, not even what was already printed.
    //  2. The scale too, so the box measured 1e8. When giving it a
    //     Rigidbody —that is, on entering Play— the inertia tensor overflowed and
    //     the PX_ASSERT of ExtInertiaTensor.h fired, which in Debug freezes the
    //     application with the CRT modal dialog and then kills it.
    //  3. And the translation, which the editor read back with getWorldTransform()
    //     and wrote into the GameObject: positions of 1e8 and lost rotations.
    //
    // In Release there would be no recognizable pattern: it would be arbitrary stack memory,
    // that is, the same failure without a value to pin it on.
    //
    // What is recovered without decomposition, which is almost everything:
    //  - The TRANSLATION is the fourth column, and it stays so even if the matrix
    //    is singular.
    //  - The SCALE is the lengths of the columns. With one axis at 0 it gives
    //    (2, 0, 2), which is the correct answer; the colliders already bound that 0
    //    to a positive minimum when baking it into their geometry.
    //  - The ROTATION is the only thing really lost, and it is that it does not exist: a
    //    flattened axis defines no orientation. Identity, which leaves the
    //    object unrotated instead of killing the editor.
    //
    // `outScale` is optional because only half of the callers use it.
    inline physx::PxTransform poseFromWorld(const glm::mat4& worldTransform,
                                            glm::vec3* outScale = nullptr)
    {
        // The safe decomposition lives in Core/TransformDecompose.h: the same
        // failure was in the inspector and in the Lua bindings, so the
        // criterion has to be ONE. Here it is only translated to PhysX.
        glm::vec3 translation{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        decomposeTransform(worldTransform, &translation, &rotation, outScale);

        const physx::PxVec3 p(translation.x, translation.y, translation.z);
        const physx::PxQuat q(rotation.x, rotation.y, rotation.z, rotation.w);

        // The belt, in case an infinity comes in through another path (a matrix with
        // a NaN inside poisons it just the same). `isSane` and `isFinite` are PhysX's ON
        // PURPOSE: the criterion by which the pose is accepted has to be
        // EXACTLY the one used by whoever will reject it, not a reimplementation
        // with std::isfinite —which would also let through a finite but
        // unnormalized quaternion, which PhysX rejects anyway—.
        return physx::PxTransform(p.isFinite() ? p : physx::PxVec3(0.0f),
                                  q.isSane()   ? q : physx::PxQuat(physx::PxIdentity));
    }
}
