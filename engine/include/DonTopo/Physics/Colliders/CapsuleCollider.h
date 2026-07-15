#pragma once
#include <glm/glm.hpp>
#include "DonTopo/Physics/Colliders/Collider.h"

namespace DonTopo {

// Componente único de física de tipo cápsula. Mismo patrón que BoxCollider
// (PxRigidDynamic siempre, useGravity alterna kinematic/dinámico). PhysX
// orienta PxCapsuleGeometry por defecto a lo largo del eje X local del
// shape; aquí se compone una rotación fija de 90° sobre Z en el localPose
// para que la "altura" quede en Y (cápsula de pie, tipo personaje).
class CapsuleCollider : public Collider {
public:
    // actor/shape ya creados por PhysicsManager, con localPose ya puesto a
    // partir de center + la rotación fija de corrección de eje.
    CapsuleCollider(void* actor, void* shape, float radius, float halfHeight,
                     const glm::vec3& center, bool useGravity);
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
    // true: actor dinámico normal. false: actor kinematic + gravedad
    // desactivada, empujado desde el GameObject vía syncTransform.
    void setUseGravity(bool enabled);

    glm::vec3 getCenter() const      { return m_center; }
    float     getRadius() const      { return m_radius; }
    float     getHalfHeight() const  { return m_halfHeight; }
    bool      getUseGravity() const  { return m_useGravity; }

    bool isDynamic() const { return m_useGravity; }

    glm::mat4 getWorldTransform() const;
    void syncTransform(const glm::mat4& worldTransform);
    void teleport(const glm::mat4& worldTransform);

protected:
    void* triggerShape() const override;

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidDynamic*
    void* m_shape = nullptr; // physx::PxShape*
#endif
    float     m_radius;
    float     m_halfHeight;
    glm::vec3 m_center;
    bool      m_useGravity;
};

} // namespace DonTopo
