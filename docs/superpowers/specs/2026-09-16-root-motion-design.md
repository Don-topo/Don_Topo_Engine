# Root motion — diseño

Fila 12 del backlog de `docs/animation-audit.md` (A6, C12). El desplazamiento horizontal que trae el hueso raíz de un clip mueve al GameObject, y la pose en GPU deja de avanzar por su cuenta. Un "andar" exportado con traslación deja de patinar.

## Decisiones

- **Rigidbody dinámico: velocidad, no transform** (elegido por el usuario, opción A). El delta horizontal se convierte en velocidad lineal en X y Z (delta/dt) y se conserva la Y actual: colisiona con paredes y la gravedad sigue actuando. Sin Rigidbody, o con uno kinematic, se mueve el transform.
- **Solo traslación horizontal.** La Y de la raíz se queda en la pose (el vaivén del paso se ve) y la rotación no se aplica al GameObject. Las dos quedan fuera de alcance.
- **Modo por estado, en un enum** y no dos bools, para que no pueda existir "aplicar sin bloquear".

## Espacio de la raíz (comprobado leyendo el código)

`ModelLoader.cpp:292-305` asigna como padre de cada hueso el hueso más cercano, y `SkinnedMeshPacking.cpp:40` da a la raíz `bindLocal = globalBind`. Los nodos que no son hueso por encima de la raíz no entran en la jerarquía de la GPU, así que las claves de la raíz están en el **espacio del modelo**, el mismo en el que se pinta la malla. El modelo se ve derecho, así que Y es arriba. Por tanto:
- delta horizontal en modelo = (dx, 0, dz) de la posición de la raíz;
- delta en mundo = `mat3(go.worldTransform) · delta_modelo` (incluye rotación y escala del GameObject).

Un test lo ancla con `assets/modelAnimation.fbx`: la Y de la raíz en t=0 es la mayor componente de su posición (altura de cadera).

## Datos (`AnimatorComponent`)

```cpp
enum class RootMotion { Off, Lock, Apply };
// en State, en lugar de bool lockRootMotion:
RootMotion rootMotion = RootMotion::Off;
```

- `poseLockRootMotion()` pasa a `uint32_t poseRootMotionMode() const`: 0 Off, 1 Lock, 2 Apply, del estado actual, como hoy.
- Nuevo miembro `double m_prevStateTicks`: el reloj acumulado del estado que se apaga en un fade. Se copia de `m_stateTicks` en `startTransitionTo` y avanza con el ritmo del estado previo.

## GPU (`shaders/bone_eval.comp`, compartido por los dos backends)

`applyRootLock`, según `push.lockRootMotion` (el nombre del campo se conserva; ahora es un modo):
- 1: posición = `bindLocal[3].xyz` (igual que hoy);
- 2: posición = (`bindLocal[3].x`, `pos.y`, `bindLocal[3].z`);
- 0: `pos` tal cual.

`setAnimationBlend` pasa el modo como `uint32_t` en lugar del bool. `EditorRenderer.h`, `Renderer`, `D3D12Renderer`, `SkinningPass` y `SkinnedFrameSync.h` solo cambian el tipo.

## Delta en CPU (`Renderer/RootMotion.h` + `.cpp`, puro y sin GPU)

```cpp
// Posición de la raíz del clip en t (ticks), interpolada linealmente entre
// posKeys como bone_eval. Sin canal de raíz o sin claves: (0,0,0).
glm::vec3 sampleRootPosition(const SkinnedMesh& mesh, int clipIndex, double t);
// Desplazamiento acumulado desde 0 hasta T ticks acumulados:
//   loop:    floor(T/D)·(P(D) − P(0)) + P(fmod(T, D)) − P(0)
//   sin loop: P(min(T, D)) − P(0)
glm::vec3 rootDisplacement(const SkinnedMesh& mesh, int clipIndex, double T, bool loop);
// Delta horizontal del último update del Animator, en espacio de modelo (y = 0).
glm::vec3 rootMotionDelta(const SkinnedMesh& mesh, const AnimatorComponent& anim);
```

La raíz es el hueso con `skeleton.parentIndex < 0` que tiene canal en el clip. Si hay varias raíces, cuenta la primera en orden topológico.

`rootMotionDelta` necesita que el Animator exponga lo avanzado en el último update:

```cpp
struct RootMotionSample { int clip; double ticks0; double ticks1; float duration; bool loop; float weight; };
const std::vector<RootMotionSample>& rootMotionSamples() const;
```

Se rellena en `update` **solo en Play** (`evaluateTransitions`) y **solo si el estado actual tiene `RootMotion::Apply`**. Se vacía al principio de cada update, igual que `firedEvents`.
- **Estado sin blend:** una muestra `{clipIndex, ticks0, m_stateTicks, duration, loop, peso}`.
- **Blend 1D:** una muestra por clip de la pareja activa (`stateBlendPair`), con ticks escalados por fase: `ticks · durClip / durPrincipal`, y peso `1 − w` para A y `w` para B (A == B: una muestra de peso 1).
- **Cross-fade:** las muestras del estado actual se multiplican por `blendWeight()`, y el estado que se apaga aporta su clip principal con `m_prevStateTicks`, peso `1 − blendWeight()`, **solo si también es Apply**. Un fade desde un estado sin Apply hacia uno con Apply arranca el movimiento de forma gradual.
- **Transición en este update:** las muestras se toman **antes** de evaluar transiciones, con el tramo del estado del que se sale, igual que los eventos.

`rootMotionDelta = Σ peso · (rootDisplacement(ticks1) − rootDisplacement(ticks0))`, con `y = 0`.

## Aplicación (`Core/RootMotionApply.h`, función libre)

```cpp
// delta en espacio de modelo del GameObject. dt en segundos.
void applyRootMotion(GameObject& go, const glm::vec3& deltaModel, float dt);
```

- `deltaModel == 0` o `dt <= 0`: nada.
- `worldDelta = mat3(go.worldTransform) · deltaModel`, y se anula su Y.
- **Rigidbody dinámico** (existe y no es kinematic): `v = rb.getVelocity(); rb.setVelocity({worldDelta.x/dt, v.y, worldDelta.z/dt})`. No toca el transform: la física lo moverá en el siguiente paso, con un frame de latencia.
- **Sin Rigidbody o kinematic:** con padre, `deltaLocal = inverse(mat3(padre.worldTransform)) · worldDelta`; sin padre, `deltaLocal = worldDelta`. `localTransform[3] += deltaLocal` y se recalcula `updateWorldTransforms()` del objeto, para que `setSkinnedTransform` de este mismo frame ya use la posición nueva.

`applySkinnedFrame` la llama tras `anim->update`, solo si `rootMotionSamples()` no está vacío, con `rootMotionDelta(*go.getSkinnedMesh(), *anim)`.

**[sin verificar]** si `Scene::update` empuja la pose de un kinematic a PhysX desde el transform. Si no lo hace, un kinematic movido así dejaría el collider atrás. El plan lo comprueba y, si hace falta, aplica el kinematic con `setKinematicTarget` o el equivalente que exista.

## Serialización (`Scene.cpp`)

- Escribe `"rootMotion": "lock"` o `"apply"`, solo si no es Off.
- Lee `rootMotion`; si falta y `lockRootMotion` es true, carga como Lock. Un valor desconocido avisa y carga Off.

## Editor

En el nodo del estado, un combo "raíz" con `normal / bloqueada / root motion`, que sustituye al checkbox actual del bloqueo. Va con popup entre `ed::Suspend()`/`ed::Resume()`, como el resto de listas del nodo. El undo lo cubre el tracker del grafo.

## Documentación

README, sección del Animator: los tres modos, y que con Rigidbody dinámico el root motion se aplica como velocidad. Audit: A6, C12 y fila 12.

## Fuera de alcance

- Rotación (giro en Y) por root motion.
- Aplicar la Y de la raíz al GameObject (saltos).
- API de Lua (`GetRootMotionDelta`, activar por script).
- Root motion durante Edit.

## Tests (uno por regla, cada uno con su sabotaje)

`animator_tests.cpp`, con un `SkinnedMesh` sintético: raíz con claves de posición lineales en X, de (0,1,0) en t=0 a (10,1,0) en t=40, y `duration` 40.
1. `rootDisplacement` en loop: 1 ciclo = 10; 2,5 ciclos = 25.
2. Sin loop: T = 100 da 10.
3. `rootMotionDelta` a través del wrap (de 38 a 42 ticks) = 1, sin salto negativo.
4. Solo X y Z: una clave con Y creciente da delta Y = 0.
5. Blend 1D con dos clips (10 y 30 por ciclo) y peso 0,5: delta = media ponderada, con ticks escalados por fase.
6. Cross-fade entre dos estados Apply con peso 0,25: 0,75·prev + 0,25·actual.
7. Estado sin Apply, o en Edit: `rootMotionSamples()` vacío.
8. `applyRootMotion` sin Rigidbody: el GameObject con escala 0,5 avanza `0,5·delta`; con un padre rotado 90° en Y, la posición local se ajusta a esa rotación.
9. `applyRootMotion` con Rigidbody dinámico: la velocidad X y Z = delta/dt y la Y se conserva (necesita un `PhysicsManager`, igual que otros tests del fichero).
10. Migración: `lockRootMotion: true` carga como Lock; ida y vuelta de `apply`.
11. Espacio: en `modelAnimation.fbx`, la raíz en t=0 tiene `|y|` mayor que `|x|` y que `|z|`.

**Verificación manual** en los dos backends: un andar con traslación en modo root motion avanza sin patinar y el vaivén vertical se ve; con Rigidbody dinámico choca con una pared; el modo "bloqueada" sigue igual que antes.
