# Auditoría del módulo de físicas (PhysX)

Fecha: 2026-08-25 · Alcance leído: `engine/include/DonTopo/Physics/**`, `engine/src/Physics/**`, `engine/tests/physics_tests.cpp`, secciones de física de `ScriptBindings.cpp`, `LuaApiReference.cpp` y `PropertiesPanel.cpp`. Lo que no salga de esos ficheros va marcado `[no verificado]`.

## 1. Inventario

### 1.1 PhysicsManager

| Capacidad | Estado | Ancla |
|---|---|---|
| Init PhysX (Foundation/Physics/Dispatcher/Scene/Material) | Sí | `PhysicsManager.cpp:111-151` |
| Escala de tolerancias en centímetros (`length=100`, `speed=981`) | Sí | `PhysicsManager.cpp:123-127` |
| Gravedad `(0,-981,0)`, no configurable en runtime | Sí (hardcodeada) | `PhysicsManager.cpp:135` |
| CPU dispatcher de 2 hilos, fijo | Sí | `PhysicsManager.cpp:130` |
| Material único compartido (staticFriction 0.5, dynamicFriction 0.5, restitution 0.1) | Sí | `PhysicsManager.cpp:142` |
| Filter shader propio: pares con trigger → `eTRIGGER_DEFAULT`; resto → shader por defecto | Sí | `PhysicsManager.cpp:87-98`, instalado en `:137` |
| `PxSimulationEventCallback` (TriggerDispatcher) sólo implementa `onTrigger` | Sí | `PhysicsManager.cpp:36-68` |
| `onContact` / `onWake` / `onSleep` / `onConstraintBreak` / `onAdvance` vacíos | Sí (no-op) | `PhysicsManager.cpp:63-67` |
| Paso de simulación `simulate()+fetchResults(true)`, sólo si `dt > 0` | Sí | `PhysicsManager.cpp:468-477` |
| Fixed step / acumulador de tiempo dentro de Physics | No: usa el `dt` crudo del caller | `PhysicsManager.cpp:473-476` |
| Síntesis de `onTriggerStay` por frame recorriendo triggers vivos | Sí | `PhysicsManager.cpp:482-492` |
| Factoría de los 4 colliders (Box/Sphere/Capsule/Plane) | Sí | `PhysicsManager.cpp:166,223,273,324` |
| Elección static vs dynamic en la creación (`bool dynamic`) | Sí | `PhysicsManager.cpp:187-189,244-246,295-297` |
| Plane siempre `PxRigidDynamic` kinematic + gravedad desactivada | Sí | `PhysicsManager.cpp:343-351` |
| `attachRigidbody` / `detachRigidbody` con reconstrucción de actor | Sí | `PhysicsManager.cpp:377-409` |
| `rebuildActor`: swap static↔dynamic preservando shape (refcount), pose y flag trigger | Sí | `PhysicsManager.cpp:412-445` |
| `userData` del actor = `Collider*` (upcast explícito) | Sí | `PhysicsManager.cpp:211-212,439` |
| `setTrigger` (flag PhysX + alta/baja en el registro de Stay) | Sí | `PhysicsManager.cpp:495-524` |
| `onColliderDestroyed`: purga punteros colgantes de los sets de overlap | Sí | `PhysicsManager.cpp:526-539` |
| Raycast simple (sin filtros) | Sí | `PhysicsManager.cpp:449-452` |
| Raycast con `PxQueryFilterData` + `PxQueryFilterCallback`, flags `ePOSITION|eNORMAL` | Sí | `PhysicsManager.cpp:454-465` |
| Guarda anti-nulo sin `PxScene` (fuera de Play) | Parcial: sólo en el raycast filtrado (`:460`); el simple deref directo | `PhysicsManager.cpp:451,460` |
| Sweep / overlap queries | No | ausente |
| Capas y máscaras de colisión (`PxFilterData` por shape) | No | ausente |
| Compilación sin PhysX (`DT_PHYSX_ENABLED` off): API completa, no-op | Sí | `PhysicsManager.cpp:214-220,264-270,315-321,369-374,478-480` |
| Shutdown ordenado (scene → callback → dispatcher → physics → foundation) | Sí | `PhysicsManager.cpp:153-164` |

### 1.2 Rigidbody

| Capacidad | Estado | Ancla |
|---|---|---|
| No posee el actor; puntero `void*` no-dueño al `PxRigidDynamic` | Sí | `Rigidbody.h:61-62` |
| `bindActor`: empuja toda la config al actor | Sí | `Rigidbody.cpp:24-39` |
| Masa (`setMassAndUpdateInertia`, recalcula inercia desde las shapes) | Sí | `Rigidbody.cpp:31,41-47` |
| Gravedad por cuerpo (`eDISABLE_GRAVITY`) + `wakeUp` al reactivar | Sí | `Rigidbody.cpp:49-60` |
| Kinemático (`PxRigidBodyFlag::eKINEMATIC`) + `wakeUp` al volver a dinámico | Sí | `Rigidbody.cpp:62-73` |
| Drag lineal (`setLinearDamping`) | Sí | `Rigidbody.cpp:75-81` |
| Drag angular (`setAngularDamping`), default 0.05 (el de Unity) | Sí | `Rigidbody.cpp:83-89`, default `Rigidbody.h:68` |
| Constraints por eje (6 bits → `PxRigidDynamicLockFlags`) | Sí | `Rigidbody.h:9-17`, `Rigidbody.cpp:8-18,91-97` |
| Velocidad lineal get/set, no-op sobre kinemático | Sí | `Rigidbody.cpp:99-117` |
| Velocidad angular get/set, no-op sobre kinemático | Sí | `Rigidbody.cpp:119-137` |
| `addForce` (`PxForceMode::eFORCE`) | Sí | `Rigidbody.cpp:139-149` |
| `addTorque` (`PxForceMode::eFORCE`) | Sí | `Rigidbody.cpp:151-161` |
| `addImpulse` (`PxForceMode::eIMPULSE`) | Sí | `Rigidbody.cpp:163-173` |
| `ForceMode` seleccionable (Acceleration / VelocityChange) | No: modos fijos por función | `Rigidbody.cpp:145,157,169` |
| API pública de sleep (`isSleeping`/`Sleep`/umbrales) | No: `wakeUp()` sólo interno | `Rigidbody.cpp:37,58,71` |
| Interpolación / extrapolación | No | ausente |
| CCD (`eENABLE_CCD`) | No | ausente |
| `MovePosition` / `MoveRotation` en el Rigidbody | No: el `setKinematicTarget` vive en el collider | `BoxCollider.cpp:110-114` |
| Centro de masa configurable | No | ausente |

### 1.3 Collider (base común)

| Capacidad | Estado | Ancla |
|---|---|---|
| Flag de trigger (`eTRIGGER_SHAPE` on / `eSIMULATION_SHAPE` off y viceversa) | Sí | `Collider.cpp:22-41` |
| Owner opaco `void*` (Physics no depende de Core) | Sí | `Collider.h:59-60` |
| Lista de `ITriggerListener` con add/remove sin duplicados | Sí | `Collider.cpp:43-54` |
| `onTriggerEnter` / `onTriggerExit` nativos de PhysX | Sí | `Collider.cpp:56-69`, origen `PhysicsManager.cpp:55-58` |
| `onTriggerStay` sintetizado por frame | Sí | `Collider.cpp:76-83` |
| Set de overlaps + limpieza silenciosa al destruirse el otro | Sí | `Collider.cpp:71-74`, `Collider.h:109` |
| Aviso al manager en el dtor | Sí | `Collider.cpp:12-20` |
| Pose polimórfica: `getWorldTransform` / `syncTransform` / `teleport` | Sí | `Collider.h:88-90` |
| Limitación documentada: PhysX no reporta trigger↔trigger | Sí (documentada) | `Collider.h:39-40` |
| Callbacks de colisión (no-trigger) | No: `onContact` vacío | `PhysicsManager.cpp:66` |

### 1.4 Los 4 colliders

| Collider | Geometría | Parámetros editables | Corrección de eje | Anclas |
|---|---|---|---|---|
| Box | `PxBoxGeometry` | `setCenter`, `setHalfExtents` | ninguna | `BoxCollider.cpp:56-74`; creación `PhysicsManager.cpp:192-195` |
| Sphere | `PxSphereGeometry` | `setCenter`, `setRadius` | ninguna | `SphereCollider.cpp:55-71`; creación `PhysicsManager.cpp:249-252` |
| Capsule | `PxCapsuleGeometry` | `setCenter`, `setRadius`, `setHalfHeight` | 90° sobre Z (altura en Y) | `CapsuleCollider.cpp:64-89`, `:17`; creación `PhysicsManager.cpp:300-303` |
| Plane | `PxPlaneGeometry` (infinito) | `setCenter` | 90° sobre Z (normal a +Y) | `PlaneCollider.cpp:61-67`, `:17`; creación `PhysicsManager.cpp:353-356` |

| Mecánica de pose | Box | Sphere | Capsule | Plane |
|---|---|---|---|---|
| `getWorldTransform` (pose→mundo, sin escala) | `BoxCollider.cpp:76-92` | `SphereCollider.cpp:74` | `CapsuleCollider.cpp:92` | `PlaneCollider.cpp:71` |
| `syncTransform` (kinematicTarget o globalPose) | `BoxCollider.cpp:94-118` | `SphereCollider.cpp:92-110` | `CapsuleCollider.cpp:110-128` | siempre `setKinematicTarget`: `PlaneCollider.cpp:89-103` |
| `teleport` (globalPose + reset de velocidad si dinámico real) | `BoxCollider.cpp:120-149` | `SphereCollider.cpp:116-132` | `CapsuleCollider.cpp:134-150` | `PlaneCollider.cpp:109-126` |
| Dtor libera el actor (`PxActor::release`) | `BoxCollider.cpp:29-36` | `SphereCollider.cpp:29-33` | `CapsuleCollider.cpp:38-42` | `PlaneCollider.cpp:35-39` |

- Escala del GameObject: ignorada. `glm::decompose` extrae `scale` pero sólo se usan traslación y rotación (`PhysicsManager.cpp:173-181`, `BoxCollider.cpp:99-107`). Escalar un objeto no escala su collider.
- Material por collider: ausente; los 4 comparten el `PxMaterial` único del manager (`PhysicsManager.cpp:193,250,301,354`).

### 1.5 Tests (`engine/tests/physics_tests.cpp`)

| Test | Qué fija | Ancla |
|---|---|---|
| `test_free_fall` | Dinámico con gravedad cae | `physics_tests.cpp:28-37` |
| `test_kinematic_no_fall` | Kinemático no cae | `physics_tests.cpp:40-50` |
| `test_freeze_position_y` | Constraint Freeze-Y | `physics_tests.cpp:53-63` |
| `test_add_impulse` | `addImpulse` cambia la velocidad | `physics_tests.cpp:66-75` |
| `test_rebuild_preserves_shape` | detach → static, geometría intacta | `physics_tests.cpp:79-90` |
| `test_trigger_needs_a_rigidbody` | static vs static → 0 eventos | `physics_tests.cpp:146-149` |
| `test_trigger_fires_when_other_has_rigidbody` | Rigidbody en el otro → dispara | `physics_tests.cpp:152-155` |
| `test_trigger_fires_when_trigger_has_rigidbody` | Rigidbody en el trigger → dispara | `physics_tests.cpp:158-161` |
| Un único `PhysicsManager` por proceso (PxFoundation) | Documentado y aplicado | `physics_tests.cpp:4-8,163-175` |
| Sin cobertura: Sphere/Capsule/Plane, raycast, Stay/Exit, drag, masa, materiales, escala | ausente | — |

### 1.6 Editor (PropertiesPanel)

| Capacidad | Estado | Ancla |
|---|---|---|
| Secciones de los 4 colliders + Rigidbody | Sí | `PropertiesPanel.cpp:419-423` |
| Add Component con gate "un solo collider por objeto" (`alreadyHasAny`) | Sí | `PropertiesPanel.cpp:7778-7816` |
| Add Rigidbody oculto si no hay collider o ya existe | Sí | `PropertiesPanel.cpp:7821-7832` |
| `setOwner(GameObject*)` al añadir el collider | Sí | `PropertiesPanel.cpp:7787,7796,7805,7814` |
| Checkbox Is Trigger por collider, vía `PhysicsManager::setTrigger` | Sí | `PropertiesPanel.cpp:6514-6517,6641-6644,6780-6783`; Plane `:6858` |
| Aviso "Sin Rigidbody: solo detecta objetos que sí lo tengan" | Sí | `PropertiesPanel.cpp:104-120`; usos `:6528,6655,6794` |
| Undo de Box Collider (center/size/trigger) | Sí | `PropertiesPanel.cpp:6452-6457,6522-6525,6551-6554` |
| Undo de Sphere Collider | Sí | `PropertiesPanel.cpp:6591-6596,6649-6652,6678-6681` |
| Undo de Capsule Collider | Sí | `PropertiesPanel.cpp:6720-6726,6788-6791,6820-6823` |
| Undo de Plane Collider | Sí | `PropertiesPanel.cpp:6854-6858` |
| Undo de Rigidbody (mass/drag/angularDrag/gravity/kinematic/constraints) | Sí | `PropertiesPanel.cpp:6964-6977,7021,7034,7046,7076` |
| Campos Mass / Drag / Angular Drag | Sí | `PropertiesPanel.cpp:7002,7006,7010` |
| Checkboxes Use Gravity / Is Kinematic | Sí | `PropertiesPanel.cpp:7028,7040` |
| Constraints: 6 checkboxes PX/PY/PZ + RX/RY/RZ | Sí | `PropertiesPanel.cpp:7052-7073` |
| Refresco de la UI desde PhysX mientras el cuerpo se simula | Sí | `PropertiesPanel.cpp:265-273,6442,6582,6710` |
| Teleport del actor al editar el Transform | Sí | `PropertiesPanel.cpp:404-414` |
| Remove Rigidbody → `detachRigidbody` antes de soltar | Sí | `PropertiesPanel.cpp:7080-7086` |
| Editor de material de física / capas | No | ausente |

### 1.7 Serialización en escena

`[no verificado]` — no hay código de serialización en `engine/src/Physics/**` ni en `engine/include/DonTopo/Physics/**` (grep de `json`/`serialize`/`toJson`: 0 resultados). Dónde y qué campos se guardan queda fuera del alcance leído.

## 2. Superficie de scripting Lua

| Símbolo Lua | Núcleo C++ | Ancla del binding |
|---|---|---|
| `BoxCollider:GetHalfExtents` / `:SetHalfExtents` | `BoxCollider::get/setHalfExtents` | `ScriptBindings.cpp:351,356` |
| `BoxCollider:GetCenter` / `:SetCenter` | `BoxCollider::get/setCenter` | `ScriptBindings.cpp:362,367` |
| `SphereCollider:GetRadius` / `:SetRadius` | `SphereCollider` | `ScriptBindings.cpp:376,381` |
| `SphereCollider:GetCenter` / `:SetCenter` | `SphereCollider` | `ScriptBindings.cpp:387,392` |
| `CapsuleCollider:GetRadius` / `:SetRadius` | `CapsuleCollider` | `ScriptBindings.cpp:401,406` |
| `CapsuleCollider:GetHalfHeight` / `:SetHalfHeight` | `CapsuleCollider` | `ScriptBindings.cpp:412,417` |
| `CapsuleCollider:GetCenter` / `:SetCenter` | `CapsuleCollider` | `ScriptBindings.cpp:423,428` |
| `PlaneCollider:GetCenter` / `:SetCenter` | `PlaneCollider` | `ScriptBindings.cpp:437,442` |
| `Rigidbody.mass` | `set/getMass` | `ScriptBindings.cpp:519-524` |
| `Rigidbody.useGravity` | `set/getUseGravity` | `ScriptBindings.cpp:526-527` |
| `Rigidbody.isKinematic` | `set/getIsKinematic` | `ScriptBindings.cpp:529-530` |
| `Rigidbody.drag` | `set/getDrag` | `ScriptBindings.cpp:532-537` |
| `Rigidbody.angularDrag` | `set/getAngularDrag` | `ScriptBindings.cpp:539-544` |
| `Rigidbody.velocity` | `set/getVelocity` | `ScriptBindings.cpp:546-551` |
| `Rigidbody.angularVelocity` | `set/getAngularVelocity` | `ScriptBindings.cpp:553-558` |
| `Rigidbody:AddForce(x,y,z)` | `addForce` | `ScriptBindings.cpp:559-564` |
| `Rigidbody:AddTorque(x,y,z)` | `addTorque` | `ScriptBindings.cpp:565-570` |
| `Rigidbody:AddImpulse(x,y,z)` | `addImpulse` | `ScriptBindings.cpp:571-576` |
| `Physics.Raycast(origin,dir,maxDist,opts)` → `{entity,point,normal,distance}` o nil | `PhysicsManager::raycast` filtrado | `ScriptBindings.cpp:2096-2115` |
| `Physics.RaycastHit(...)` → bool | ídem sin construir tabla | `ScriptBindings.cpp:2119-2129` |
| Opciones `hitTriggers`, `static`, `dynamic`, `ignore` | `PxQueryFilterData` + `RaycastFilter` | `ScriptBindings.cpp:2035-2054,2075-2083`; filtro `:1932-1962` |
| `GetComponent("BoxCollider"/"SphereCollider"/"CapsuleCollider"/"PlaneCollider"/"Rigidbody")` | — | `ScriptBindings.cpp:1711-1716` |
| `AddComponent(...)` de los 4 colliders + Rigidbody (mismos gates que el editor) | — | `ScriptBindings.cpp:1748-1784` |
| `RemoveComponent(...)` (Rigidbody hace `detachRigidbody` antes) | — | `ScriptBindings.cpp:1874-1901` |
| Callbacks `OnTriggerEnter` / `OnTriggerStay` / `OnTriggerExit` | `ITriggerListener` | declarados en `LuaApiReference.cpp:34-37`; dónde se registra el listener queda fuera del alcance leído → `[no verificado]` |
| Validación anti-NaN/inf en los setters numéricos (`ensureFinite`) | — | `ScriptBindings.cpp:359,384,409,522,535,562,568,574` |

### En el core C++ pero NO expuesto a Lua

| Capacidad C++ | Ancla | Nota |
|---|---|---|
| `Rigidbody::setConstraints` / `getConstraints` | `Rigidbody.cpp:91-97` | grep de `constraints` en `ScriptBindings.cpp` y `LuaApiReference.cpp`: 0 resultados |
| `PhysicsManager::setTrigger` (Is Trigger en runtime) | `PhysicsManager.cpp:495` | sólo lo usa el editor (`PropertiesPanel.cpp:6517`) |
| `Collider::isTrigger` (lectura) | `Collider.h:55` | ausente en Lua |
| `Collider::teleport` / `syncTransform` / `getWorldTransform` | `Collider.h:88-90` | ausente en Lua |
| `Collider::addListener` / `removeListener` | `Collider.cpp:43-54` | ausente en Lua |
| `PhysicsManager::raycast` sin filtros | `PhysicsManager.cpp:449` | Lua siempre usa la variante filtrada |
| `PhysicsManager::attach/detachRigidbody` directos | `PhysicsManager.cpp:377,393` | sólo vía Add/RemoveComponent |

## 3. Comparativa con Unity

| Capacidad Unity | Estado aquí | Evidencia |
|---|---|---|
| Rigidbody: masa | Sí | `Rigidbody.cpp:41-47` |
| Rigidbody: drag / angularDrag | Sí | `Rigidbody.cpp:75-89` |
| Rigidbody: useGravity | Sí | `Rigidbody.cpp:49-60` |
| Rigidbody: isKinematic | Sí | `Rigidbody.cpp:62-73` |
| Rigidbody: constraints de ejes (6) | Sí (no en Lua) | `Rigidbody.cpp:91-97`; UI `PropertiesPanel.cpp:7052-7073` |
| Rigidbody: interpolación / extrapolación | No | ausente |
| Rigidbody: detección continua (CCD) | No | ausente |
| Rigidbody: `AddForce`/`AddTorque` con `ForceMode` | Parcial: 3 funciones de modo fijo (Force/Force/Impulse); faltan Acceleration y VelocityChange | `Rigidbody.cpp:139-173` |
| Rigidbody: sleep (`IsSleeping`/`WakeUp`/`Sleep`, umbrales) | No (sólo `wakeUp()` interno) | `Rigidbody.cpp:37,58,71` |
| Rigidbody kinemático con `MovePosition`/`MoveRotation` | Parcial: existe `setKinematicTarget` vía `Collider::syncTransform`, pero sin API en `Rigidbody` ni en Lua | `BoxCollider.cpp:110-114`, `PlaneCollider.cpp:103` |
| Physics Materials (fricción/rebote por objeto, combine modes) | No: un único `PxMaterial` global 0.5/0.5/0.1 | `PhysicsManager.cpp:142`; uso `:193,250,301,354` |
| BoxCollider / SphereCollider / CapsuleCollider | Sí | `PhysicsManager.cpp:166,223,273` |
| Plane infinito (sin equivalente directo en Unity) | Sí | `PhysicsManager.cpp:324` |
| MeshCollider convexo | No | ausente |
| Escala del Transform aplicada al collider | No: `decompose` descarta `scale` | `PhysicsManager.cpp:173-181`, `BoxCollider.cpp:99-107` |
| Varios colliders por objeto (compound) | No: uno por GameObject | `PropertiesPanel.cpp:7778-7781`, `ScriptBindings.cpp:1748` |
| Capas y matriz de colisión | No: nunca se escribe `PxFilterData` por shape | ausente |
| `Physics.Raycast` | Sí (con `ignore`, `hitTriggers`, `static`, `dynamic`) | `ScriptBindings.cpp:2096`; núcleo `PhysicsManager.cpp:454` |
| `Physics.RaycastAll` (multi-hit) | No: sólo hit de bloqueo | `ScriptBindings.cpp:2101-2109` |
| `Physics.SphereCast` / `CapsuleCast` (sweeps) | No | ausente |
| `Physics.OverlapSphere` / `OverlapBox` | No | ausente |
| `OnTriggerEnter` / `Stay` / `Exit` | Sí (Stay sintetizado en CPU) | `Collider.cpp:56-83`, `PhysicsManager.cpp:482-492`; Lua `LuaApiReference.cpp:34-37` |
| `OnCollisionEnter` / `Stay` / `Exit` | No: `onContact` vacío y sin `eNOTIFY_TOUCH_*` de contacto en el shader | `PhysicsManager.cpp:66,96-97` |
| Trigger↔trigger detectado | No (limitación de PhysX, documentada) | `Collider.h:39-40` |
| Regla "al menos un Rigidbody" avisada al usuario | Sí | `PropertiesPanel.cpp:104-120`; tests `physics_tests.cpp:146-161` |
| Joints (Fixed / Hinge / Spring / Configurable) | No | ausente |
| CharacterController | No | ausente |
| `Time.fixedDeltaTime` configurable + acumulador de paso fijo | No: `stepSimulation(dt)` usa el `dt` que reciba | `PhysicsManager.cpp:468-477` |
| Gravedad global configurable | No: hardcodeada a `-981` | `PhysicsManager.cpp:135` |
| Solver iterations por cuerpo | No | ausente |
| Debug visual de colliders (gizmos) | `[no verificado]` — el dibujado no vive en `engine/src/Physics/**`, fuera del alcance leído | — |
| Serialización en escena de colliders/rigidbody | `[no verificado]` — sin código de serialización en `Physics/` | — |
| Undo en el editor de colliders y Rigidbody | Sí | `PropertiesPanel.cpp:6551-6554,6678-6681,6820-6823,6854-6858,7021-7077` |

## 4. Sugerencias (orden valor/coste)

1. **Materiales de física por collider** (fricción estática/dinámica y rebote). Un `PxMaterial` por collider en vez del global, con setters `setFriction`/`setBounciness` y campos en la sección del collider.
   Toca: `PhysicsManager.cpp:142,193,250,301,354`, `Collider.h/.cpp`, bindings de Lua, sección de collider en `PropertiesPanel.cpp`.
   El material es interno a la shape y nadie fuera de Physics lo lee; la persistencia de los 3 floats sí exige el serializador de escena.

2. **Constraints y `isTrigger` expuestos a Lua**: `Rigidbody.constraints` (bitmask ya existente) y `Collider.isTrigger` get/set contra `PhysicsManager::setTrigger`.
   Toca: `ScriptBindings.cpp:349-446` y `:516-576`, listado en `LuaApiReference.cpp:86-100`.
   No afecta al resto: el núcleo ya existe (`Rigidbody.cpp:91`, `PhysicsManager.cpp:495`), es sólo superficie de binding.

3. **`ForceMode` en AddForce/AddTorque** (Force / Acceleration / Impulse / VelocityChange) como 4º argumento opcional, default = comportamiento actual.
   Toca: `Rigidbody.h:57-59`, `Rigidbody.cpp:139-173`, bindings `ScriptBindings.cpp:559-576`.
   No afecta al resto: retrocompatible, y nadie más llama a estas funciones.

4. **Paso fijo con acumulador en `stepSimulation`** más `fixedDeltaTime` y `maxSubSteps`; hoy la simulación depende del framerate.
   Toca: sólo `PhysicsManager.cpp:468-477` y `PhysicsManager.h:54`.
   No afecta al resto: la firma se mantiene y el caller sigue pasando el `dt` real. El Stay pasaría a emitirse por sub-step (decisión a documentar).

5. **`Physics.RaycastAll` (multi-hit)** con `PxRaycastBufferN` y `eNO_BLOCK`, devolviendo lista ordenada por distancia.
   Toca: sobrecarga en `PhysicsManager.cpp:454-465`, `doRaycast` en `ScriptBindings.cpp:2061-2084`, tabla de retorno `:2104-2110`.
   No afecta al resto: reutiliza `RaycastArgs`, `RaycastFilter` y `actorOwner` tal cual.

6. **Sweeps y overlaps** (`SphereCast`, `OverlapSphere`, `OverlapBox`) con `PxScene::sweep`/`overlap` y los mismos filtros que el raycast.
   Toca: métodos nuevos en `PhysicsManager.h/.cpp` junto a `:449-465`, y `registerPhysics` ampliada (`ScriptBindings.cpp:2087-2130`).
   No afecta al resto: son consultas de sólo lectura sobre la `PxScene`.

7. ~~**Capas de colisión con máscara**~~ — HECHO: 32 capas, `PxFilterData` por shape (`word0 = 1<<capa`, `word1 = máscara`), matriz simétrica en `PhysicsManager` con capas creadas/borradas a demanda (`addLayer`/`removeLayer`, techo de 32, borrar compacta y reasigna a la 0), comprobación al principio de `dtTriggerFilterShader`, UI de collider + ventana `Collision Layers`, bindings de Lua y persistencia en los `settings` del project.json.
   Toca: `PhysicsManager.cpp:87-98`, las 4 factorías (`:193,250,301,354`), `Collider.h` (layer), bindings y UI del collider.
   Sí afecta fuera: la matriz global necesita persistirse en los settings del proyecto.

1. **Callbacks de colisión (`OnCollisionEnter/Stay/Exit`)**: implementar `onContact` y pedir `eNOTIFY_TOUCH_FOUND/LOST/PERSISTS` para pares no-trigger, con una interfaz gemela de `ITriggerListener`.
   Toca: `PhysicsManager.cpp:66,96-97`, `Collider.h:24-30`, `Collider.cpp:56-83`.
   No afecta al resto: mismo camino `userData → Collider → owner` que ya usan los triggers, y el coste de CPU sólo aparece si se activan las flags.

2. **CCD e interpolación por Rigidbody**: `eENABLE_CCD` en el cuerpo más `PxSceneFlag::eENABLE_CCD` en la escena; la interpolación como suavizado de la pose leída en `getWorldTransform`.
   Toca: `PhysicsManager.cpp:134-140` (sceneDesc), `Rigidbody.h/.cpp`, dos checkboxes en `PropertiesPanel.cpp:7028-7048`.
   No afecta al resto: la interpolación se resuelve dentro del collider, sin tocar el renderer.

3.  **Colliders que respeten la escala del Transform**, reescalando la geometría con el `scale` que hoy se descarta.
    Toca: `PhysicsManager.cpp:173-181` y el `syncTransform` de los 4 colliders (`BoxCollider.cpp:94-118` y gemelos).
    No afecta al resto: es conversión interna. Aviso: cambia el comportamiento de escenas existentes con objetos escalados.
