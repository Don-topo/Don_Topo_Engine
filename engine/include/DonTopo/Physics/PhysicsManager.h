#pragma once
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
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

    // --- Capas de colisión ---------------------------------------------------
    //
    // 32 capas fijas (índice 0-31, el ancho de PxFilterData::word0). La 0 se
    // llama "Default"; el resto nacen sin nombre. La matriz de colisión es
    // SIMÉTRICA y arranca entera a true: con los defaults, cada shape lleva
    // word0 = 1<<capa y word1 = todo a unos, ningún par se suprime, y la
    // simulación es exactamente la de antes de existir las capas (triggers
    // incluidos).
    // kLayerCount es el TECHO (el ancho de word0), no cuántas hay: las capas se
    // crean y se borran desde el editor y las vivas son siempre el prefijo
    // [0, layerCount()). La 0 existe siempre y no se puede borrar.
    static constexpr int kLayerCount = 32;
    static bool isValidLayer(int layer) { return layer >= 0 && layer < kLayerCount; }

    int  layerCount() const { return m_layerCount; }

    // Crea una capa al final. Devuelve su índice, o -1 si ya hay kLayerCount.
    int  addLayer(const std::string& name);

    // Borra la capa y COMPACTA: los colliders que la usaban pasan a la 0, los de
    // capas superiores bajan un índice, y la matriz pierde su fila y su columna
    // (la última queda liberada y vuelve a "colisiona con todo"). Devuelve false
    // pa la capa 0 y pa un índice que no exista.
    //
    // OJO: renumera. Un script que guarde índices de capa a pelo apunta a otra
    // capa después de un borrado — es el precio de que la lista no tenga huecos.
    bool removeLayer(int layer);

    // Activa/desactiva la colisión entre dos capas. Escribe las DOS mitades de
    // la matriz (a-b y b-a) y recalcula el word1 de todos los colliders vivos,
    // así el cambio vale también en mitad de una partida. Índice inválido:
    // no-op.
    void setLayerCollision(int a, int b, bool enabled);
    // false pa un índice inválido (no hay fila que consultar).
    bool getLayerCollision(int a, int b) const;

    // Nombre editable de la capa; puramente informativo (UI y project.json), el
    // filtrado va siempre por índice. Índice inválido: no-op / cadena vacía.
    void        setLayerName(int layer, const std::string& name);
    std::string getLayerName(int layer) const;

    // Máscara de la capa: bit b a 1 = 'layer' colisiona con la capa b. Es
    // literalmente el word1 que se escribe en el PxFilterData de sus shapes.
    // Índice inválido: 0.
    uint32_t layerMask(int layer) const;

    // Reescribe el PxFilterData de la shape del collider desde su capa actual.
    // Lo llaman Collider::setLayer (guardar el número no basta: el filtro que
    // mira PhysX vive en la shape) y las 4 factorías al crear. No-op sin PhysX,
    // sin shape o con capa fuera de rango.
    void refreshColliderFilter(Collider* collider);

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

    // Multi-hit: añade PxQueryFlag::eNO_BLOCK a filterData, así todo impacto se
    // reporta como touch y la consulta no para en el primero. hits tiene que
    // ser un PxRaycastBufferN<N> (el PxRaycastBuffer a secas no lleva
    // almacenamiento de touches y solo recogería el bloqueante); los touches se
    // devuelven ORDENADOS por distancia ascendente — PhysX los entrega sin
    // orden. Si el buffer se llena, PhysX trunca en silencio: el caller lo
    // detecta con getNbTouches() == getMaxNbTouches(). Mismos hit flags y misma
    // guarda de escena ausente que raycast().
    bool raycastAll(const physx::PxVec3& origin, const physx::PxVec3& dir, float maxDistance,
                    physx::PxRaycastBuffer& hits, const physx::PxQueryFilterData& filterData,
                    physx::PxQueryFilterCallback* filterCall);

    // Barrido de una esfera de radio 'radius' desde origin a lo largo de dir:
    // el rayo "con grosor" que hace falta para mover un personaje sin que se
    // cuele por las esquinas. Single-hit (el primero que bloquea), mismos hit
    // flags que raycast() para que el binding devuelva la misma tabla. Si la
    // esfera ya solapa algo en el origen, PhysX reporta distancia 0 y el punto/
    // normal no son fiables (no se pide eMTD). Misma guarda de escena ausente.
    bool sphereCast(const physx::PxVec3& origin, const physx::PxVec3& dir, float radius,
                    float maxDistance, physx::PxSweepBuffer& hit,
                    const physx::PxQueryFilterData& filterData,
                    physx::PxQueryFilterCallback* filterCall);

    // Qué shapes solapan una esfera estática en 'center'. Multi-hit: añade
    // PxQueryFlag::eNO_BLOCK igual que raycastAll, si no el primer eBLOCK del
    // prefiltro cerraría la consulta. hits tiene que ser un PxOverlapBufferN<N>;
    // al llenarse, PhysX trunca en silencio (getNbTouches() ==
    // getMaxNbTouches()). Sin orden: un overlap no tiene distancia.
    bool overlapSphere(const physx::PxVec3& center, float radius,
                       physx::PxOverlapBuffer& hits,
                       const physx::PxQueryFilterData& filterData,
                       physx::PxQueryFilterCallback* filterCall);

    // Igual pero con una caja orientada (halfExtents + rotación del mundo).
    bool overlapBox(const physx::PxVec3& center, const physx::PxVec3& halfExtents,
                    const physx::PxQuat& rotation, physx::PxOverlapBuffer& hits,
                    const physx::PxQueryFilterData& filterData,
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

    // Alta en m_colliders + primer volcado del PxFilterData. La llaman las 4
    // factorías justo tras setManager().
    void registerCollider(const std::shared_ptr<Collider>& collider);

    // Recalcula el PxFilterData de TODOS los colliders vivos. La matriz es
    // global pero el filtro está copiado en cada shape, así que un cambio de
    // matriz obliga a reescribirlas todas.
    void refreshAllColliderFilters();

    // Todos los colliders creados por este manager (weak: los dueños son los
    // GameObjects). Sólo sirve pa recalcular filtros; se poda en
    // onColliderDestroyed, que corre en cada muerte de collider.
    std::vector<std::weak_ptr<Collider>> m_colliders;

    // Matriz de colisión, comprimida a una máscara por capa: bit b de
    // m_layerMasks[a] = "a colisiona con b". Arranca entera a unos.
    std::array<uint32_t, kLayerCount> m_layerMasks = [] {
        std::array<uint32_t, kLayerCount> m{};
        m.fill(0xFFFFFFFFu);
        return m;
    }();

    // Capas vivas: siempre >= 1 (la 0 no se borra).
    int m_layerCount = 1;

    // Nombres de capa. Sólo la 0 nace nombrada ("Default"), como en Unity.
    std::array<std::string, kLayerCount> m_layerNames = [] {
        std::array<std::string, kLayerCount> n;
        n[0] = "Default";
        return n;
    }();

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
