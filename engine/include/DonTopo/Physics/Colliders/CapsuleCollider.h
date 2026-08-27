#pragma once
#include <glm/glm.hpp>
#include "DonTopo/Physics/Colliders/Collider.h"

namespace DonTopo {

// Componente único de física de tipo cápsula. Mismo patrón que BoxCollider
// (PxRigidStatic sin Rigidbody, PxRigidDynamic con Rigidbody). PhysX orienta
// PxCapsuleGeometry por defecto a lo largo del eje X local del shape; aquí se
// compone una rotación fija de 90° sobre Z en el localPose para que la
// "altura" quede en Y (cápsula de pie, tipo personaje).
class CapsuleCollider : public Collider {
public:
    // actor/shape ya creados por PhysicsManager, con localPose ya puesto a
    // partir de center + la rotación fija de corrección de eje.
    CapsuleCollider(void* actor, void* shape, float radius, float halfHeight,
                     const glm::vec3& center);
    ~CapsuleCollider();

    CapsuleCollider(const CapsuleCollider&)            = delete;
    CapsuleCollider& operator=(const CapsuleCollider&) = delete;

    // Offset local de la shape dentro del actor. Reaplica siempre la
    // rotación fija de corrección de eje junto con la traslación.
    void setCenter(const glm::vec3& center);
    // Radio de la cápsula (PxShape::setGeometry con nueva PxCapsuleGeometry).
    void setRadius(float radius);
    // Medio-alto de la cápsula (distancia entre los centros de las dos
    // semiesferas; PxShape::setGeometry con nueva PxCapsuleGeometry).
    void setHalfHeight(float halfHeight);
    // Escala del Transform del GameObject. PxTransform no admite escala, así
    // que se hornea en la geometría: el radio va con el mayor de los dos ejes
    // transversales (max(abs(x), abs(z)) — la cápsula está de pie en Y por la
    // rotación de corrección) y el medio-alto con abs(y). m_radius/m_halfHeight
    // —lo que ve el inspector y lo que se serializa— no cambian. Idempotente:
    // con la misma escala no toca nada.
    void setWorldScale(const glm::vec3& scale);

    glm::vec3 getCenter() const      { return m_center; }
    float     getRadius() const      { return m_radius; }
    float     getHalfHeight() const  { return m_halfHeight; }

    void* actorHandle() const override;
    void  setActorHandle(void* actor) override;

    glm::mat4 getWorldTransform() const override;
    void syncTransform(const glm::mat4& worldTransform) override;
    void teleport(const glm::mat4& worldTransform) override;

protected:
    void* triggerShape() const override;

private:
#ifdef DT_PHYSX_ENABLED
    // Sube a PhysX m_radius/m_halfHeight con m_worldScale ya aplicada.
    // Requiere m_shape.
    void applyScaledGeometry();

    void* m_actor = nullptr; // physx::PxRigidActor* (static o dynamic)
    void* m_shape = nullptr; // physx::PxShape*
#endif
    float     m_radius;
    float     m_halfHeight;
    glm::vec3 m_center;
    // m_worldScale vive en la base Collider; ver nota en BoxCollider.h.
};

} // namespace DonTopo
