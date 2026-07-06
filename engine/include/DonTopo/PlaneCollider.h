#pragma once
#include <glm/glm.hpp>

namespace DonTopo {

// Componente único de física de tipo plano infinito. A diferencia de
// Box/Sphere/Capsule, siempre es kinematic + gravedad desactivada (un plano
// "cayendo" no tiene sentido físico) — no expone toggle Use Gravity.
// isDynamic() retorna false hardcoded: el motor siempre empuja la pose del
// GameObject hacia PhysX (syncTransform), nunca lee de vuelta.
class PlaneCollider {
public:
    // actor/shape ya creados por PhysicsManager, con localPose ya puesto a
    // partir de center + la rotación fija que mapea la normal por defecto a
    // +Y (mismo truco de eje que CapsuleCollider).
    PlaneCollider(void* actor, void* shape, const glm::vec3& center);
    ~PlaneCollider();

    PlaneCollider(const PlaneCollider&)            = delete;
    PlaneCollider& operator=(const PlaneCollider&) = delete;

    // Offset local de la shape dentro del actor. Reaplica siempre la
    // rotación fija de corrección de eje junto con la traslación.
    void setCenter(const glm::vec3& center);

    glm::vec3 getCenter() const { return m_center; }
    bool isDynamic() const { return false; }

    glm::mat4 getWorldTransform() const;
    void syncTransform(const glm::mat4& worldTransform);
    void teleport(const glm::mat4& worldTransform);

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidDynamic*
    void* m_shape = nullptr; // physx::PxShape*
#endif
    glm::vec3 m_center;
};

} // namespace DonTopo
