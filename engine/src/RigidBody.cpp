#include "DonTopo/RigidBody.h"

#ifdef DT_PHYSX_ENABLED
#define GLM_ENABLE_EXPERIMENTAL
#include <PxPhysicsAPI.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

using namespace physx;
#endif

namespace DonTopo {

RigidBody::RigidBody(void* actor)
{
#ifdef DT_PHYSX_ENABLED
    m_actor = actor;
#else
    (void)actor;
#endif
}

RigidBody::~RigidBody()
{
#ifdef DT_PHYSX_ENABLED
    if (m_actor) static_cast<PxRigidDynamic*>(m_actor)->release();
#endif
}

glm::mat4 RigidBody::getWorldTransform() const
{
#ifdef DT_PHYSX_ENABLED
    if (!m_actor) return glm::mat4(1.0f);

    PxTransform pose = static_cast<PxRigidDynamic*>(m_actor)->getGlobalPose();

    glm::mat4 translation = glm::translate(glm::mat4(1.0f),
        glm::vec3(pose.p.x, pose.p.y, pose.p.z));
    glm::quat rotation(pose.q.w, pose.q.x, pose.q.y, pose.q.z);
    glm::mat4 rotationMat = glm::mat4_cast(rotation);

    return translation * rotationMat;
#else
    return glm::mat4(1.0f);
#endif
}

void RigidBody::setWorldTransform(const glm::mat4& worldTransform)
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

    auto* actor = static_cast<PxRigidDynamic*>(m_actor);
    actor->setGlobalPose(pose);
    actor->setLinearVelocity(PxVec3(0.0f));
    actor->setAngularVelocity(PxVec3(0.0f));
#else
    (void)worldTransform;
#endif
}

} // namespace DonTopo
