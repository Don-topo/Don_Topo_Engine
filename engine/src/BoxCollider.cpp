#include "DonTopo/BoxCollider.h"

#ifdef DT_PHYSX_ENABLED
#define GLM_ENABLE_EXPERIMENTAL
#include <PxPhysicsAPI.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

using namespace physx;
#endif

namespace DonTopo {

BoxCollider::BoxCollider(void* actor, const glm::vec3& halfExtents)
    : m_halfExtents(halfExtents)
{
#ifdef DT_PHYSX_ENABLED
    m_actor = actor;
#else
    (void)actor;
#endif
}

BoxCollider::~BoxCollider()
{
#ifdef DT_PHYSX_ENABLED
    if (m_actor) static_cast<PxRigidStatic*>(m_actor)->release();
#endif
}

void BoxCollider::syncTransform(const glm::mat4& worldTransform)
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return;

    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(worldTransform, scale, rotation, translation, skew, perspective);

    PxTransform pose(
        PxVec3(translation.x, translation.y, translation.z),
        PxQuat(rotation.x, rotation.y, rotation.z, rotation.w)
    );
    static_cast<PxRigidStatic*>(m_actor)->setGlobalPose(pose);
#else
    (void)worldTransform;
#endif
}

} // namespace DonTopo
