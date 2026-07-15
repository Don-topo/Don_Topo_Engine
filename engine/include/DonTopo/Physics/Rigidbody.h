#pragma once
#include <glm/glm.hpp>
#include <cstdint>

namespace DonTopo {

// Constraints estilo Unity: congelar traslación/rotación por eje. Bitmask que
// se traduce a physx::PxRigidDynamicLockFlags en bindActor().
enum RigidbodyConstraints : uint32_t {
    RB_None            = 0,
    RB_FreezePositionX = 1u << 0,
    RB_FreezePositionY = 1u << 1,
    RB_FreezePositionZ = 1u << 2,
    RB_FreezeRotationX = 1u << 3,
    RB_FreezeRotationY = 1u << 4,
    RB_FreezeRotationZ = 1u << 5,
};

// Componente de dinámica de cuerpo rígido (equivalente a Unity Rigidbody). NO
// posee el actor PhysX: lo posee el Collider (mismo contrato de vida de
// siempre). Este componente guarda un puntero NO-dueño al PxRigidDynamic y
// actúa como "config + API". Agnóstico de GameObject: la dependencia va
// Core -> Physics, nunca al revés. No incluye PxPhysicsAPI.h para no filtrar
// PhysX en headers alcanzables desde GameObject.h.
class Rigidbody {
public:
    Rigidbody() = default;
    Rigidbody(const Rigidbody&)            = delete;
    Rigidbody& operator=(const Rigidbody&) = delete;

    // Guarda el actor (physx::PxRigidDynamic* como void*) y empuja TODA la
    // config actual al actor. Lo llama PhysicsManager tras crear/reconstruir el
    // actor dinámico. Sin DT_PHYSX_ENABLED solo guarda el puntero.
    void  bindActor(void* actor);
    void* actor() const { return m_actor; }

    // Config (los setters escriben al actor enlazado si existe).
    float getMass() const        { return m_mass; }
    void  setMass(float mass);
    bool  getUseGravity() const  { return m_useGravity; }
    void  setUseGravity(bool enabled);
    bool  getIsKinematic() const { return m_isKinematic; }
    void  setIsKinematic(bool enabled);
    float getDrag() const        { return m_drag; }
    void  setDrag(float drag);
    float getAngularDrag() const { return m_angularDrag; }
    void  setAngularDrag(float drag);
    uint32_t getConstraints() const { return m_constraints; }
    void     setConstraints(uint32_t mask);

    // Dinámica. Velocidad y fuerzas son no-op si el actor es kinematic (PhysX
    // las ignora / avisa); se guardan/aplican solo cuando tiene sentido.
    glm::vec3 getVelocity() const;
    void      setVelocity(const glm::vec3& v);
    glm::vec3 getAngularVelocity() const;
    void      setAngularVelocity(const glm::vec3& v);
    void addForce(const glm::vec3& f);   // PxForceMode::eFORCE
    void addTorque(const glm::vec3& t);  // PxForceMode::eFORCE
    void addImpulse(const glm::vec3& f); // PxForceMode::eIMPULSE

private:
    void* m_actor = nullptr; // physx::PxRigidDynamic* (no-dueño)

    float    m_mass        = 1.0f;
    bool     m_useGravity  = true;
    bool     m_isKinematic = false;
    float    m_drag        = 0.0f;
    float    m_angularDrag = 0.05f; // default de Unity
    uint32_t m_constraints = RB_None;
};

} // namespace DonTopo
