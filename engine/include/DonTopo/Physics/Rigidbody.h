#pragma once
#include <glm/glm.hpp>
#include <cstdint>

namespace DonTopo {

// Unity-style constraints: freeze translation/rotation per axis. Bitmask that
// is translated to physx::PxRigidDynamicLockFlags in bindActor().
enum RigidbodyConstraints : uint32_t {
    RB_None            = 0,
    RB_FreezePositionX = 1u << 0,
    RB_FreezePositionY = 1u << 1,
    RB_FreezePositionZ = 1u << 2,
    RB_FreezeRotationX = 1u << 3,
    RB_FreezeRotationY = 1u << 4,
    RB_FreezeRotationZ = 1u << 5,
};

// Force application mode, Unity style. It is translated to physx::PxForceMode
// in Rigidbody.cpp: Force/Acceleration are integrated during the step (they depend on
// dt), Impulse/VelocityChange change the velocity at once. The two on the
// right (Acceleration/VelocityChange) IGNORE the body's mass.
enum class ForceMode {
    Force,          // continuous, depends on mass and dt (default, as before)
    Acceleration,   // continuous, ignores mass
    Impulse,        // instantaneous, depends on mass
    VelocityChange, // instantaneous, ignores mass
};

// Rigid body dynamics component (equivalent to Unity Rigidbody). It does NOT
// own the PhysX actor: the Collider owns it (same lifetime contract as
// always). This component stores a NON-owning pointer to the PxRigidDynamic and
// acts as "config + API". GameObject-agnostic: the dependency goes
// Core -> Physics, never the other way. It does not include PxPhysicsAPI.h so as not to leak
// PhysX into headers reachable from GameObject.h.
class Rigidbody {
public:
    Rigidbody() = default;
    Rigidbody(const Rigidbody&)            = delete;
    Rigidbody& operator=(const Rigidbody&) = delete;

    // Stores the actor (physx::PxRigidDynamic* as void*) and pushes ALL the current
    // config to the actor. Called by PhysicsManager after creating/rebuilding the
    // dynamic actor. Without DT_PHYSX_ENABLED it only stores the pointer.
    void  bindActor(void* actor);
    void* actor() const { return m_actor; }

    // Config (the setters write to the linked actor if it exists).
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

    // CCD (Continuous Collision Detection). With the fixed step, a fast body
    // can pass through a thin wall in a single sub-step: the discrete test
    // compares initial and final poses and between them there is nothing. With CCD, PhysX
    // sweeps the body's trajectory inside the sub-step and detects the impact.
    //
    // Default OFF, as in Unity: it costs CPU time and is only needed for
    // projectiles / very fast bodies. The scene is already born with
    // PxSceneFlag::eENABLE_CCD, but that only enables the pass; without this per-body
    // flag no actor uses it and the simulation is the usual one.
    //
    // PhysX does not support CCD on kinematic bodies (a kinematic does not need it: the
    // solver does not move it). Setting it here stores the intent and the flag is set
    // on the actor only while the body is not kinematic.
    bool getCcd() const { return m_ccd; }
    void setCcd(bool enabled);

    // VISUAL interpolation of the pose between fixed steps. It is not resolved by the
    // Rigidbody: it is applied by the Collider (which has the pose and is the one
    // asked for getWorldTransform); only the property lives here, which is what the
    // user sees and what is serialized. Independent of CCD.
    bool getInterpolate() const { return m_interpolate; }
    void setInterpolate(bool enabled);

    // Dynamics. Velocity and forces are no-ops if the actor is kinematic (PhysX
    // ignores them / warns); they are stored/applied only when it makes sense.
    glm::vec3 getVelocity() const;
    void      setVelocity(const glm::vec3& v);
    glm::vec3 getAngularVelocity() const;
    void      setAngularVelocity(const glm::vec3& v);
    // The mode is optional and defaults to Force: single-argument
    // calls behave exactly as before.
    void addForce(const glm::vec3& f, ForceMode mode = ForceMode::Force);
    void addTorque(const glm::vec3& t, ForceMode mode = ForceMode::Force);
    void addImpulse(const glm::vec3& f); // sugar for ForceMode::Impulse

private:
    void* m_actor = nullptr; // physx::PxRigidDynamic* (non-owning)

    float    m_mass        = 1.0f;
    bool     m_useGravity  = true;
    bool     m_isKinematic = false;
    float    m_drag        = 0.0f;
    float    m_angularDrag = 0.05f; // default de Unity
    uint32_t m_constraints = RB_None;
    bool     m_ccd         = false; // OFF: does not change any existing scene
    bool     m_interpolate = false; // OFF: getWorldTransform gives the raw pose
};

} // namespace DonTopo
