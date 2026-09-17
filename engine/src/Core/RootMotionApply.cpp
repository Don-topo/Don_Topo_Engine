#include "DonTopo/Core/RootMotionApply.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Physics/Rigidbody.h"

namespace DonTopo
{
    void applyRootMotion(GameObject& go, const glm::vec3& deltaModel, float dt)
    {
        if (dt <= 0.0f || deltaModel == glm::vec3(0.0f)) return;
        // Rotación y escala del objeto: el clip avanza hacia donde mira y a su
        // tamaño. La Y se anula en MUNDO, no en modelo: un objeto inclinado no
        // debe despegar del suelo.
        glm::vec3 mundo = glm::mat3(go.worldTransform) * deltaModel;
        mundo.y = 0.0f;

        if (const auto& rb = go.getRigidbody(); rb && !rb->getIsKinematic())
        {
            const glm::vec3 v = rb->getVelocity();
            rb->setVelocity(glm::vec3(mundo.x / dt, v.y, mundo.z / dt));
            return;
        }

        const glm::mat4 padre = go.parent ? go.parent->worldTransform : glm::mat4(1.0f);
        go.localTransform[3] += glm::vec4(glm::inverse(glm::mat3(padre)) * mundo, 0.0f);
        // Este mismo frame: setSkinnedTransform viene justo después.
        go.updateWorldTransforms(padre);
    }
}
