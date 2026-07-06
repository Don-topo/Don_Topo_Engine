#pragma once
#include <glm/glm.hpp>

namespace DonTopo {

// Componente único de física de tipo caja. Siempre respaldado por un
// physx::PxRigidDynamic (nunca PxRigidStatic): con useGravity=false el actor
// queda kinematic + gravedad desactivada (se mueve empujado desde el
// GameObject); con useGravity=true PhysX lo simula normal y su pose se lee
// de vuelta hacia el GameObject cada frame. Togglear useGravity solo cambia
// flags del actor existente, nunca lo destruye/recrea.
class BoxCollider {
public:
    // actor: physx::PxRigidDynamic* ya creado y añadido a la escena por
    // PhysicsManager. shape: physx::PxShape* de geometría caja adjunta a ese
    // actor, con localPose ya puesto a partir de center.
    BoxCollider(void* actor, void* shape, const glm::vec3& halfExtents,
                const glm::vec3& center, bool useGravity);
    ~BoxCollider();

    BoxCollider(const BoxCollider&)            = delete;
    BoxCollider& operator=(const BoxCollider&) = delete;

    // Offset local de la shape dentro del actor (PxShape::setLocalPose).
    void setCenter(const glm::vec3& center);
    // Medio-tamaño de la caja (PxShape::setGeometry con nueva PxBoxGeometry).
    void setHalfExtents(const glm::vec3& halfExtents);
    // true: actor dinámico normal (cae con la gravedad de la escena).
    // false: actor kinematic + gravedad desactivada (no cae, se empuja
    // desde el GameObject vía syncTransform).
    void setUseGravity(bool enabled);

    glm::vec3 getCenter() const       { return m_center; }
    glm::vec3 getHalfExtents() const  { return m_halfExtents; }
    bool      getUseGravity() const   { return m_useGravity; }

    // true si el motor debe LEER la pose de PhysX hacia el GameObject
    // (getWorldTransform); false si debe EMPUJAR la pose del GameObject
    // hacia PhysX (syncTransform).
    bool isDynamic() const { return m_useGravity; }

    // Lee physx::PxRigidDynamic::getGlobalPose() (traslación + rotación,
    // sin escala). Solo válido cuando isDynamic() es true.
    glm::mat4 getWorldTransform() const;

    // Empuja worldTransform hacia PhysX vía setKinematicTarget (traslación +
    // rotación; la escala se ignora). Solo válido cuando isDynamic() es
    // false.
    void syncTransform(const glm::mat4& worldTransform);

    // Teletransporta el actor (setGlobalPose, no setKinematicTarget) sea
    // cual sea el modo, y resetea su velocidad a cero. Válido en ambos
    // modos (dinámico o kinematic) — pensado para ediciones puntuales desde
    // el Transform panel del editor, no para el empuje continuo por frame
    // (eso es syncTransform).
    void teleport(const glm::mat4& worldTransform);

private:
#ifdef DT_PHYSX_ENABLED
    void* m_actor = nullptr; // physx::PxRigidDynamic*
    void* m_shape = nullptr; // physx::PxShape*
#endif
    glm::vec3 m_halfExtents;
    glm::vec3 m_center;
    bool      m_useGravity;
};

} // namespace DonTopo
