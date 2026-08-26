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

// Modo de aplicación de fuerzas, estilo Unity. Se traduce a physx::PxForceMode
// en Rigidbody.cpp: Force/Acceleration se integran durante el paso (dependen de
// dt), Impulse/VelocityChange cambian la velocidad de golpe. Los dos de la
// derecha (Acceleration/VelocityChange) IGNORAN la masa del cuerpo.
enum class ForceMode {
    Force,          // continua, dependiente de masa y dt (default, como antes)
    Acceleration,   // continua, ignora la masa
    Impulse,        // instantánea, dependiente de masa
    VelocityChange, // instantánea, ignora la masa
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

    // CCD (Continuous Collision Detection). Con el paso fijo, un cuerpo rápido
    // puede atravesar una pared fina en un solo sub-step: el test discreto
    // compara poses inicial y final y entre ellas no hay nada. Con CCD, PhysX
    // barre la trayectoria del cuerpo dentro del sub-step y detecta el impacto.
    //
    // Default OFF, como en Unity: cuesta tiempo de CPU y sólo hace falta en
    // proyectiles / cuerpos muy rápidos. La escena ya nace con
    // PxSceneFlag::eENABLE_CCD, pero eso sólo habilita el pase; sin este flag
    // por cuerpo ningún actor lo usa y la simulación es la de siempre.
    //
    // PhysX no admite CCD en cuerpos kinematic (un kinematic no lo necesita: no
    // lo mueve el solver). Marcarlo aquí guarda la intención y el flag se pone
    // en el actor sólo mientras el cuerpo no sea kinematic.
    bool getCcd() const { return m_ccd; }
    void setCcd(bool enabled);

    // Interpolación VISUAL de la pose entre pasos fijos. No la resuelve el
    // Rigidbody: la aplica el Collider (que es quien tiene la pose y a quien se
    // le pide getWorldTransform); aquí vive sólo la propiedad, que es lo que el
    // usuario ve y lo que se serializa. Independiente de CCD.
    bool getInterpolate() const { return m_interpolate; }
    void setInterpolate(bool enabled);

    // Dinámica. Velocidad y fuerzas son no-op si el actor es kinematic (PhysX
    // las ignora / avisa); se guardan/aplican solo cuando tiene sentido.
    glm::vec3 getVelocity() const;
    void      setVelocity(const glm::vec3& v);
    glm::vec3 getAngularVelocity() const;
    void      setAngularVelocity(const glm::vec3& v);
    // El modo es opcional y por defecto Force: las llamadas de un solo
    // argumento se comportan exactamente igual que antes.
    void addForce(const glm::vec3& f, ForceMode mode = ForceMode::Force);
    void addTorque(const glm::vec3& t, ForceMode mode = ForceMode::Force);
    void addImpulse(const glm::vec3& f); // azúcar de ForceMode::Impulse

private:
    void* m_actor = nullptr; // physx::PxRigidDynamic* (no-dueño)

    float    m_mass        = 1.0f;
    bool     m_useGravity  = true;
    bool     m_isKinematic = false;
    float    m_drag        = 0.0f;
    float    m_angularDrag = 0.05f; // default de Unity
    uint32_t m_constraints = RB_None;
    bool     m_ccd         = false; // OFF: no cambia ninguna escena existente
    bool     m_interpolate = false; // OFF: getWorldTransform da la pose cruda
};

} // namespace DonTopo
