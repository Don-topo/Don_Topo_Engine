#pragma once
#include <vector>
#include <unordered_set>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

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

// Evento de COLISIÓN: el par no es trigger y PhysX lo resuelve de verdad
// (impulsos, rebote). Mismos campos y misma semántica de `other` opaco que
// TriggerEvent.
//
// NO lleva puntos de contacto a propósito: PhysX los da
// (PxContactPair::extractContacts) pero exige pedir
// eNOTIFY_CONTACT_POINTS en el filter shader, lo que copia el stream de
// contactos de CADA par a memoria del callback. Hoy nadie en el motor
// —scripting, editor, gameplay— consume posiciones de contacto, así que se
// paga ese coste el día que haga falta.
struct CollisionEvent {
    void*     other         = nullptr;
    Collider* otherCollider = nullptr;
};

// Gemelo de ITriggerListener para pares no-trigger. Métodos vacíos por
// defecto, igual que allí.
//
// Diferencia con los triggers: aquí el Stay NO se sintetiza por frame. PhysX
// emite eNOTIFY_TOUCH_PERSISTS de forma nativa en onContact mientras el
// contacto siga vivo, así que no hace falta registro ni recorrido de overlaps.
class ICollisionListener {
public:
    virtual ~ICollisionListener() = default;
    virtual void onCollisionEnter(const CollisionEvent&) {}
    virtual void onCollisionStay (const CollisionEvent&) {}
    virtual void onCollisionExit (const CollisionEvent&) {}
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

    // Listeners de colisión (pares no-trigger). Lista aparte de la de
    // triggers: un collider no puede ser las dos cosas a la vez, pero el mismo
    // consumidor sí puede registrarse en ambas y no queremos que un evento de
    // un tipo recorra los listeners del otro.
    void addCollisionListener(ICollisionListener* listener);
    void removeCollisionListener(ICollisionListener* listener);

    // Material de física POR COLLIDER. Los defaults son los mismos que tenía el
    // PxMaterial global anterior (0.5 / 0.5 / 0.1), así que una escena que no
    // guarde estos campos simula exactamente igual que antes.
    //
    // Los setters escriben al PxMaterial de la shape (que es exclusivo de este
    // collider: lo crea PhysicsManager en la factoría). Sin DT_PHYSX_ENABLED
    // solo guardan el valor, para que el editor y la serialización funcionen
    // igual en un build sin PhysX.
    void setFriction(float staticF, float dynamicF);
    void setBounciness(float restitution);

    float getStaticFriction() const  { return m_staticFriction; }
    float getDynamicFriction() const { return m_dynamicFriction; }
    float getBounciness() const      { return m_restitution; }

    // Capa de colisión del collider (0-31, la 0 es "Default"). Con quién
    // colisiona esa capa lo decide la matriz del PhysicsManager.
    //
    // El setter NO se limita a guardar el número: PhysX filtra por el
    // PxFilterData de la shape, así que hay que reescribirlo
    // (PhysicsManager::refreshColliderFilter). Un índice fuera de [0,31] se
    // ignora y conserva la capa anterior — 1u<<32 es UB y una capa inventada no
    // tiene fila en la matriz.
    void setLayer(int layer);
    int  getLayer() const { return m_layer; }

    // --- Interpolación de la pose (Rigidbody.interpolate) --------------------
    //
    // La física corre a paso fijo y el render va al ritmo del frame: entre dos
    // sub-steps la pose del actor NO cambia, y a 144 Hz con la física a 60 se
    // ve a tirones. Con interpolación, getWorldTransform no devuelve la pose
    // cruda del actor sino la mezcla entre la pose previa al último sub-step y
    // la actual, según cuánto del siguiente paso lleve consumido el acumulador.
    //
    // Es puramente VISUAL: no toca lo que simula PhysX, así que un raycast o un
    // trigger siguen viendo la pose real. Default OFF -> getWorldTransform
    // devuelve exactamente lo de siempre.
    //
    // Lo enciende Rigidbody::setInterpolate (que llega hasta aquí por el
    // userData del actor): la propiedad la expone el Rigidbody, la mecánica
    // vive en el collider, que es quien tiene la pose.
    void setInterpolate(bool enabled);
    bool getInterpolate() const { return m_interpolate; }

    // Guarda la pose del actor ANTES del sub-step; la llama
    // PhysicsManager::stepSimulation una vez por sub-step. No-op si este
    // collider no interpola.
    void capturePreviousPose();

    // Fracción [0,1] del paso fijo ya consumida. La empuja
    // PhysicsManager::stepSimulation al final del frame.
    void setInterpolationAlpha(float alpha);

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

    // Colisiones, invocadas por el mismo dispatcher desde onContact. No hay
    // bookkeeping que llevar: los tres los emite PhysX
    // (eNOTIFY_TOUCH_FOUND/PERSISTS/LOST) y aquí solo se reparten.
    void dispatchCollisionEnter(Collider* other);
    void dispatchCollisionStay (Collider* other);
    void dispatchCollisionExit (Collider* other);

protected:
    // Mezcla `current` (la pose real del actor, que acaba de leer el collider
    // concreto) con la pose previa capturada, usando el alpha del acumulador.
    // Devuelve `current` tal cual si la interpolación está apagada o aún no hay
    // pose previa. Lo llaman los getWorldTransform de los colliders con
    // Rigidbody posible (box/sphere/capsule); el plano es siempre estático.
    glm::mat4 blendWithPreviousPose(const glm::mat4& current) const;

    // Devuelve physx::PxShape* del collider concreto, como void* para no filtrar
    // PhysX en este header (que llega hasta Core vía GameObject.h). Sin
    // DT_PHYSX_ENABLED devuelve nullptr.
    virtual void* triggerShape() const = 0;

private:
    bool                           m_isTrigger = false;
    // Defaults idénticos al PxMaterial global que había antes (0.5/0.5/0.1).
    float                          m_staticFriction  = 0.5f;
    float                          m_dynamicFriction = 0.5f;
    float                          m_restitution     = 0.1f;
    int                            m_layer     = 0; // "Default"
    // Interpolación visual. m_hasPrevPose evita mezclar contra una pose previa
    // que nunca se capturó (el primer frame tras encenderla).
    bool                           m_interpolate  = false;
    bool                           m_hasPrevPose  = false;
    float                          m_interpAlpha  = 1.0f;
    glm::vec3                      m_prevPosition{0.0f};
    glm::quat                      m_prevRotation{1.0f, 0.0f, 0.0f, 0.0f};
    void*                          m_owner     = nullptr;
    PhysicsManager*                m_manager   = nullptr;
    std::vector<ITriggerListener*> m_listeners;
    std::vector<ICollisionListener*> m_collisionListeners;
    std::unordered_set<Collider*>  m_overlaps;
};

} // namespace DonTopo
