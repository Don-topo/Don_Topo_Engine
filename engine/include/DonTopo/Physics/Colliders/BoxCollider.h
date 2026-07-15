#pragma once
#include <glm/glm.hpp>
#include "DonTopo/Physics/Colliders/Collider.h"

namespace DonTopo {

// Componente único de física de tipo caja. Respaldado por un physx::PxRigidStatic
// (sin Rigidbody) o un physx::PxRigidDynamic (con Rigidbody). El collider POSEE
// su actor (lo libera en el dtor); PhysicsManager reconstruye el actor al
// pasar de static a dynamic o viceversa (attach/detachRigidbody). La política
// de gravedad/kinematic ya NO vive aquí: vive en el Rigidbody.
class BoxCollider : public Collider {
public:
    // actor: physx::PxRigidStatic* o PxRigidDynamic* ya creado y añadido a la
    // escena por PhysicsManager. shape: physx::PxShape* de geometría caja
    // adjunta a ese actor, con localPose ya puesto a partir de center.
    BoxCollider(void* actor, void* shape, const glm::vec3& halfExtents,
                const glm::vec3& center);
    ~BoxCollider();

    BoxCollider(const BoxCollider&)            = delete;
    BoxCollider& operator=(const BoxCollider&) = delete;

    // Offset local de la shape dentro del actor (PxShape::setLocalPose).
    void setCenter(const glm::vec3& center);
    // Medio-tamaño de la caja (PxShape::setGeometry con nueva PxBoxGeometry).
    void setHalfExtents(const glm::vec3& halfExtents);

    glm::vec3 getCenter() const       { return m_center; }
    glm::vec3 getHalfExtents() const  { return m_halfExtents; }

    void* actorHandle() const override;
    void  setActorHandle(void* actor) override;

    // Lee la pose global del actor (traslación + rotación, sin escala). El
    // motor la lee hacia el GameObject cuando hay un Rigidbody simulado.
    glm::mat4 getWorldTransform() const;

    // Empuja worldTransform hacia PhysX. Si el actor es dynamic-kinematic usa
    // setKinematicTarget; en cualquier otro caso cae a setGlobalPose.
    void syncTransform(const glm::mat4& worldTransform);

    // Teletransporta el actor (setGlobalPose, no setKinematicTarget) sea
    // cual sea el modo, y resetea su velocidad a cero. Válido en ambos
    // modos (dinámico o kinematic) — pensado para ediciones puntuales desde
    // el Transform panel del editor, no para el empuje continuo por frame
    // (eso es syncTransform).
    void teleport(const glm::mat4& worldTransform);

protected:
    void* triggerShape() const override;

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidActor* (static o dynamic)
    void* m_shape = nullptr; // physx::PxShape*
#endif
    glm::vec3 m_halfExtents;
    glm::vec3 m_center;
};

} // namespace DonTopo
