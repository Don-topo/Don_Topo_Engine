#pragma once
#include <vector>
#include <unordered_set>
#include <glm/glm.hpp>

namespace DonTopo {

class Collider;
class PhysicsManager;

// Evento de trigger estilo Unity. `other` es un handle OPACO (void*) al dueño
// del collider que provocó el evento: el módulo de física es agnóstico de
// GameObject (la dependencia va Core -> Physics, nunca al revés), así que Core
// setea el owner vía Collider::setOwner y editor/scripting lo resuelven a
// GameObject más adelante. `otherCollider` es el collider concreto que solapó.
struct TriggerEvent {
    void*     other         = nullptr;
    Collider* otherCollider = nullptr;
};

// Interfaz de callbacks de trigger. Métodos vacíos por defecto: el consumidor
// sobrescribe solo los que necesita (igual que OnTriggerEnter/Stay/Exit de
// Unity).
class ITriggerListener {
public:
    virtual ~ITriggerListener() = default;
    virtual void onTriggerEnter(const TriggerEvent&) {}
    virtual void onTriggerStay (const TriggerEvent&) {}
    virtual void onTriggerExit (const TriggerEvent&) {}
};

// Base común de los 4 colliders (Box/Sphere/Capsule/Plane). Aporta el estado
// de trigger, el owner opaco, los listeners y el set de overlaps.
//
// PhysX reporta Enter/Exit de forma nativa (eNOTIFY_TOUCH_FOUND/LOST vía
// PxSimulationEventCallback::onTrigger); Stay NO lo da PhysX y se sintetiza
// cada frame recorriendo m_overlaps (lo hace PhysicsManager::stepSimulation).
//
// Limitación de PhysX: no reporta overlaps trigger<->trigger; al menos un lado
// del par debe ser no-trigger para recibir eventos.
class Collider {
public:
    virtual ~Collider();

    Collider() = default;
    Collider(const Collider&)            = delete;
    Collider& operator=(const Collider&) = delete;

    // Marca/desmarca la shape como trigger en PhysX (pone eTRIGGER_SHAPE y
    // quita eSIMULATION_SHAPE: una shape no puede ser ambas). NO gestiona el
    // alta en el registro de Stay del PhysicsManager — usar
    // PhysicsManager::setTrigger como entry point público, que llama a esto y
    // actualiza el registro.
    void applyTriggerFlag(bool enabled);
    bool isTrigger() const { return m_isTrigger; }

    // Owner opaco. Lo setea Core al enlazar collider <-> GameObject; el módulo
    // de física nunca lo desreferencia, solo lo transporta en TriggerEvent.
    void  setOwner(void* owner) { m_owner = owner; }
    void* getOwner() const      { return m_owner; }

    void addListener(ITriggerListener* listener);
    void removeListener(ITriggerListener* listener);

    // Lo setea PhysicsManager al crear el collider, para que ~Collider pueda
    // avisar de su destrucción y purgarse de los overlaps de otros triggers.
    void setManager(PhysicsManager* manager) { m_manager = manager; }

    // Actor PhysX subyacente (PxRigidStatic* o PxRigidDynamic*), como void*.
    // Lo usa PhysicsManager::rebuildActor pa reasignar el actor tras cambiar
    // de tipo (static <-> dynamic). El collider sigue siendo el DUEÑO: lo
    // libera en su dtor.
    virtual void* actorHandle() const = 0;
    virtual void  setActorHandle(void* actor) = 0;

    // physx::PxShape* del collider concreto (como void*), reutiliza el
    // triggerShape() interno. Lo usa PhysicsManager::rebuildActor pa
    // re-adjuntar la MISMA shape al nuevo actor tras el swap static<->dynamic.
    void* geometryShape() const { return triggerShape(); }

    // Mecánica de pose del actor, polimórfica: la recorre Scene::update sobre
    // el collider base (anyCollider) sin ramificar por tipo concreto. Cada
    // collider las implementa idénticas sobre su m_actor.
    //   getWorldTransform: lee pose actor -> mundo (cuerpo simulado).
    //   syncTransform:     empuja mundo -> actor (setKinematicTarget si
    //                      dynamic-kinematic; setGlobalPose en otro caso).
    //   teleport:          setGlobalPose + reset de velocidad si dynamic real.
    virtual glm::mat4 getWorldTransform() const = 0;
    virtual void      syncTransform(const glm::mat4& worldTransform) = 0;
    virtual void      teleport(const glm::mat4& worldTransform) = 0;

    // Bookkeeping de overlaps, invocado por el dispatcher de PhysicsManager.
    void beginOverlap(Collider* other);        // TOUCH_FOUND: inserta + onTriggerEnter
    void endOverlap(Collider* other);          // TOUCH_LOST : borra   + onTriggerExit
    void removeOverlapSilent(Collider* other); // limpieza sin disparar (destrucción del otro)
    void dispatchStay();                        // por frame: onTriggerStay de cada overlap vivo

protected:
    // Devuelve physx::PxShape* del collider concreto, como void* para no filtrar
    // PhysX en este header (que llega hasta Core vía GameObject.h). Sin
    // DT_PHYSX_ENABLED devuelve nullptr.
    virtual void* triggerShape() const = 0;

private:
    bool                           m_isTrigger = false;
    void*                          m_owner     = nullptr;
    PhysicsManager*                m_manager   = nullptr;
    std::vector<ITriggerListener*> m_listeners;
    std::unordered_set<Collider*>  m_overlaps;
};

} // namespace DonTopo
