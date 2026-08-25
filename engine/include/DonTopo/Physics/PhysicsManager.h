#pragma once
#include <glm/glm.hpp>
#include <memory>
#include <vector>

#ifdef DT_PHYSX_ENABLED
#include <PxPhysicsAPI.h>
#endif

namespace DonTopo { class Collider; class BoxCollider; class SphereCollider; class CapsuleCollider; class PlaneCollider; class Rigidbody; }

namespace DonTopo {

class PhysicsManager {
public:
    PhysicsManager() = default;
    ~PhysicsManager();
    PhysicsManager(const PhysicsManager&)            = delete;
    PhysicsManager& operator=(const PhysicsManager&) = delete;

    void init();
    void shutdown();

    // dynamic=false -> PxRigidStatic (collider sin Rigidbody); dynamic=true ->
    // PxRigidDynamic (collider con Rigidbody, la config la aplica luego
    // attachRigidbody -> Rigidbody::bindActor).
    std::shared_ptr<BoxCollider> createBoxColliderComponent(const glm::vec3& halfExtents,
                                                              const glm::vec3& center,
                                                              const glm::mat4& worldTransform,
                                                              bool dynamic);

    std::shared_ptr<SphereCollider> createSphereColliderComponent(float radius,
                                                                    const glm::vec3& center,
                                                                    const glm::mat4& worldTransform,
                                                                    bool dynamic);

    std::shared_ptr<CapsuleCollider> createCapsuleColliderComponent(float radius,
                                                                      float halfHeight,
                                                                      const glm::vec3& center,
                                                                      const glm::mat4& worldTransform,
                                                                      bool dynamic);

    // Plane: siempre static/kinematic, nunca lleva Rigidbody (firma sin dynamic).
    std::shared_ptr<PlaneCollider> createPlaneColliderComponent(const glm::vec3& center,
                                                                  const glm::mat4& worldTransform);

    // Asegura que el actor del collider sea PxRigidDynamic (reconstruye si era
    // static), luego enlaza el Rigidbody (rb->bindActor). Collider WITHOUT
    // Rigidbody = static; WITH = dynamic.
    void attachRigidbody(const std::shared_ptr<Collider>& collider, const std::shared_ptr<Rigidbody>& rb);
    // Reconstruye el actor del collider como PxRigidStatic (deshace attach).
    void detachRigidbody(const std::shared_ptr<Collider>& collider);

    void stepSimulation(float dt);

    // Paso fijo: el dt real del frame se acumula y se consume en trozos de
    // m_fixedDeltaTime, así la simulación no depende del framerate.
    // <= 0 se ignora (mantiene el valor anterior): un 0 colgaría el bucle
    // que resta el paso del acumulador.
    void  setFixedDeltaTime(float dt);
    float getFixedDeltaTime() const { return m_fixedDeltaTime; }

    // Techo de sub-steps por llamada; se clampea a >= 1 (con 0 la física no
    // avanzaría nunca). Lo que sobre del acumulador tras agotarlos se tira.
    void setMaxSubSteps(int steps);
    int  getMaxSubSteps() const { return m_maxSubSteps; }

    // Marca/desmarca un collider como trigger: flip de flags PhysX
    // (Collider::applyTriggerFlag) + alta/baja en el registro que se recorre
    // cada frame para sintetizar onTriggerStay. Entry point público — lo
    // llamará Core al integrar editor/scripting.
    void setTrigger(const std::shared_ptr<Collider>& collider, bool enabled);

    // Llamado por ~Collider: purga el collider de los sets de overlap de todos
    // los triggers vivos, evitando punteros colgantes antes del próximo Stay.
    void onColliderDestroyed(Collider* collider);

#ifdef DT_PHYSX_ENABLED
    bool raycast(const physx::PxVec3& origin, const physx::PxVec3& dir, float maxDistance, physx::PxRaycastBuffer& hit);

    // Misma consulta con filtros: filterData elige qué actores se recorren
    // (eSTATIC / eDYNAMIC) y filterCall descarta shapes una a una (triggers,
    // actor a ignorar). Pide ePOSITION|eNORMAL, que es lo que consume el
    // binding de Lua. Devuelve false sin tocar PhysX si aún no hay PxScene
    // (fuera de Play).
    bool raycast(const physx::PxVec3& origin, const physx::PxVec3& dir, float maxDistance,
                 physx::PxRaycastBuffer& hit, const physx::PxQueryFilterData& filterData,
                 physx::PxQueryFilterCallback* filterCall);
#endif

private:
#ifdef DT_PHYSX_ENABLED
    // Cambia el tipo de actor del collider (static<->dynamic) preservando shape,
    // pose y estado trigger. Devuelve el nuevo PxRigidActor* como void*.
    void* rebuildActor(const std::shared_ptr<Collider>& collider, bool dynamic);
#endif

    // Recorre m_triggerColliders emitiendo onTriggerStay y podando expirados.
    // Se llama una vez por sub-step (ver stepSimulation).
    void dispatchTriggerStay();

    // Triggers registrados (weak: los GameObjects poseen los colliders vía
    // shared_ptr). Se recorren cada frame para emitir onTriggerStay; los
    // expirados se podan al vuelo.
    std::vector<std::weak_ptr<Collider>> m_triggerColliders;

    // Acumulador de tiempo del paso fijo. PxScene::simulate con el dt real del
    // frame hace la física no determinista (el mismo escenario cae distinto a
    // 60 y a 144 fps) y con un frame largo —carga de assets, breakpoint— el
    // integrador da un salto enorme y los cuerpos se atraviesan. Guardando el
    // sobrante y simulando siempre trozos de m_fixedDeltaTime, el resultado
    // sólo depende del tiempo total transcurrido.
    float m_fixedDeltaTime = 1.0f / 60.0f;
    int   m_maxSubSteps    = 8;
    float m_accumulator    = 0.0f;

#ifdef DT_PHYSX_ENABLED
    void* m_foundation      = nullptr; // physx::PxFoundation*
    void* m_physics         = nullptr; // physx::PxPhysics*
    void* m_scene           = nullptr; // physx::PxScene*
    void* m_dispatcher      = nullptr; // physx::PxDefaultCpuDispatcher*
    void* m_triggerCallback = nullptr; // TriggerDispatcher* (PxSimulationEventCallback)
#endif
};

} // namespace DonTopo
