# IK del Animator: look-at y dos huesos (fila 15, C13)

Primera de las cuatro features que la fila 15 de `docs/animation-audit.md`
agrupaba ("diferidas hasta tener el buffer de pose"). Las otras tres —clips de
propiedades (C14), curvas de clip (C9) y sub-máquinas (C10)— van aparte, cada
una con su spec. Base: la pose por muestras (13), el blend 2D (14a) y las
capas (14b).

## Qué se construye

Restricciones de IK en el Animator, resueltas en la GPU entre la jerarquía y
el skinning:

- **LookAt**: gira un hueso (cabeza, torso) para llevar un eje suyo hacia un
  objetivo, con ángulo máximo y peso.
- **TwoBone**: resuelve una cadena de tres huesos (hombro-codo-mano,
  cadera-rodilla-pie) para que el extremo llegue a un objetivo, con un *pole*
  que fija hacia dónde apunta el codo o la rodilla, y peso.

Decisiones tomadas:

- Los dos resolvedores en la misma entrega: comparten toda la fontanería.
- Se resuelve en la GPU, en un pase nuevo. Ni en CPU (duplicaría la
  evaluación de `bone_eval`: muestreo, mezcla, capas y additive) ni por
  readback (sincroniza o va un frame por detrás).
- La corrección se escribe en las transformaciones **locales**: la segunda
  pasada de jerarquía la propaga a los hijos.

## 1. Datos (`AnimatorComponent`)

```cpp
enum class IkType { LookAt, TwoBone };
static constexpr int kMaxIkConstraints = 4;
struct IkConstraint
{
    std::string name;              // identificador para Lua y el panel
    IkType      type   = IkType::LookAt;
    // LookAt: el hueso que mira. TwoBone: el EXTREMO de la cadena (mano, pie);
    // los otros dos huesos son su padre y su abuelo en el esqueleto.
    std::string boneName;
    uint64_t    targetId = 0;      // GameObject objetivo; 0 = sin objetivo
    uint64_t    poleId   = 0;      // TwoBone: GameObject del pole; 0 = sin pole
    float       weight   = 1.0f;   // 0..1
    glm::vec3   aimAxis  = { 0.0f, 0.0f, 1.0f };   // LookAt: eje local que mira
    float       maxAngle = 80.0f;  // LookAt: grados, acotado a [0, 180]
    // --- resuelto en bindClips, no se serializa ---
    int boneIndex = -1, parentIndex = -1, grandParentIndex = -1;
};
```

- Viven en el componente (no por capa): la IK se aplica sobre la pose final.
- `bindClips`/`rebindClips` resuelven `boneName` contra `skeleton.names` y, en
  TwoBone, el padre y el abuelo con `skeleton.parentIndex`. Un nombre que no
  existe, o una cadena con menos de tres huesos, deja `boneIndex = -1` y empuja
  un aviso, como un clip que no existe: la restricción no se aplica.
- API: `ikConstraints()`, `ikConstraintsMutable()`, `addIkConstraint(c)`
  (devuelve el índice, −1 si ya hay `kMaxIkConstraints`),
  `removeIkConstraint(i)`, `setIkWeight(nombre, w)` (acotado a [0,1]),
  `ikWeight(nombre)`, `setIkTarget(nombre, id)`, `setIkPole(nombre, id)`.
  Los que buscan por nombre no hacen nada si no existe (como los parámetros).

## 2. Lo que llega al backend

```cpp
static constexpr int kMaxIkPose = 4;
struct IkSolve
{
    uint32_t  type   = 0;          // 0 LookAt, 1 TwoBone
    int       bone = -1, parent = -1, grandParent = -1;
    float     weight = 0.0f;
    glm::vec3 target{ 0.0f };      // en espacio del MODELO
    glm::vec3 pole{ 0.0f };        // ídem; sin pole, hasPole = 0
    uint32_t  hasPole = 0;
    glm::vec3 aimAxis{ 0.0f, 0.0f, 1.0f };
    float     maxAngle = 80.0f;    // grados
};
struct AnimationIk { IkSolve solves[kMaxIkPose]; int count = 0; };
```

`applySkinnedFrame`, que es donde está el GameObject:

1. Por cada restricción con `boneIndex >= 0`, `weight > 0` y `targetId != 0`:
   busca el objetivo subiendo a la raíz de la escena y buscando por id (el
   mismo criterio que `Scene::findById`, sin necesitar la escena).
2. Pasa el objetivo (y el pole) al espacio del modelo con la inversa del
   `worldTransform` del personaje.
3. `renderer.setAnimationIk(go.skinnedRenderIndex, ik)` — siempre, también con
   `count = 0`, para que quitar la última restricción apague la IK.

**Limitación asumida y documentada**: se supone escala uniforme en el
personaje. Con escala no uniforme, la inversa deforma las longitudes de la
cadena y la IK no cuadra.

## 3. GPU

- **Bloque de IK** por personaje, con una copia por frame en vuelo y su offset
  en el push constant, igual que el bloque de pose (`PoseBlock.h`). Layout en
  `IkBlock.h` (uints; floats por sus bits): `[0] count`, `[1..3] relleno`, y
  por restricción, en `4 + 16k`: `type, bone, parent, grandParent, weight,
  targetX, targetY, targetZ, poleX, poleY, poleZ, hasPole, aimX, aimY, aimZ,
  maxAngle`.
- El push constant de los tres `.comp` gana dos uints, `ikBlockOffset` y
  `flags` (de 16 a 24 bytes), espejados en `SkinningPass::Push` y
  `ComputePush`.
- `bone_hierarchy.comp` mira el bit 0 de `flags` ("solo mundo"): con él salta
  la pasada 2, y `finalBones` queda con los transforms de mundo.
- `bone_ik.comp` (nuevo, un hilo por restricción, un workgroup por personaje):
  lee los mundos de `finalBones` y escribe los **locales** corregidos en
  `localXforms`.
  - **LookAt**: dirección actual = rotación de mundo del hueso × `aimAxis`;
    deseada = normalizar(objetivo − posición del hueso). El giro entre las dos
    se acota a `maxAngle` y se escala por `weight` (slerp desde la identidad).
    El local nuevo conserva posición y escala; solo cambia la rotación.
    Objetivo pegado al hueso (distancia < 1e-5) o eje nulo: no toca nada.
  - **TwoBone**: A = abuelo, B = padre, C = extremo. Con las longitudes
    actuales |AB| y |BC| y la distancia de A al objetivo, la ley de cosenos da
    el ángulo de la articulación; el plano lo fija el pole (sin pole, el que ya
    tenía la cadena). El objetivo más lejos que |AB| + |BC| deja la cadena
    estirada hacia él. Las dos rotaciones resultantes se interpolan desde las
    originales con `weight`, y se escriben como locales de A y B. El extremo
    conserva su local.
- Orden por personaje: evaluar clips → jerarquía → skinning, como hoy, **salvo
  si tiene IK activa**: evaluar clips → jerarquía (solo mundo) → `bone_ik` →
  jerarquía → skinning. Los dos pases extra van en sus propias fases, para no
  perder lo de la fila 9 (una barrera por fase, no por personaje).

## 4. Editor

Sección **IK** en el panel del Animator, bajo la barra de capas (fuera del
lienzo del grafo), con `PushID` propio:

- Lista de restricciones con su nombre editable; **+** (desactivado en 4) y
  **−**.
- Por restricción: combo de tipo, combo del hueso con los nombres del
  esqueleto (en rojo si no resuelve), selector de GameObject para objetivo y
  pole (lista por nombre de la escena, con "(ninguno)"), `SliderFloat` de peso
  y, solo en LookAt, los tres campos del eje y el ángulo máximo.
- Todo pasa por el undo del grafo: la clave sale de `animatorToJson`.

## 5. Serialización

- Solo si hay restricciones: `"ik": [ { "name", "type": "lookAt"|"twoBone",
  "bone", "target", "pole", "weight", "aimAxis": [x,y,z], "maxAngle" } ]`.
  `target` y `pole` son ids de GameObject, como los guarda el resto de la
  escena.
- Una escena sin IK se guarda idéntica, byte a byte.
- Más de `kMaxIkConstraints` en el fichero: se cargan las primeras y se avisa.
- Un `type` desconocido carga como LookAt y avisa.

## 6. Lua

`SetIkWeight(nombre, peso)`, `GetIkWeight(nombre)` (0 si no existe),
`SetIkTarget(nombre, entidad)`, `SetIkPole(nombre, entidad)` y
`GetIkCount()`. Con `LuaApiReference.cpp` y la sección `## Lua Scripting` del
README.

## 7. Pruebas

En `engine/tests/animator_tests.cpp`, TDD y sabotajes de uno en uno. La
referencia en CPU replica `bone_ik.comp` paso a paso, como `evalLayeredTrs`
replica a `bone_eval`:

- **LookAt**: con el objetivo delante, el eje del hueso acaba apuntando al
  objetivo (ángulo < 1e-4); con el objetivo a 150° y `maxAngle = 80`, el giro
  aplicado es exactamente 80°; `weight = 0.5` gira la mitad; `weight = 0` no
  cambia el local; objetivo sobre el propio hueso no cambia nada.
- **TwoBone**: objetivo alcanzable → componiendo la cadena desde los locales
  que devuelve, el extremo queda en el objetivo (< 1e-4); objetivo a más
  distancia que |AB| + |BC| → cadena estirada y el extremo en la dirección del
  objetivo; dos poles opuestos dan codos en lados distintos del plano;
  `weight = 0` deja la cadena como estaba; objetivo en la propia A no rompe
  (sin NaN).
- **Resolución**: nombre de hueso que no existe avisa y deja la restricción
  inactiva; cadena de menos de tres huesos, lo mismo.
- **Bloque**: `writeIkBlock` con dos restricciones escribe los 16 uints de cada
  una en su offset.
- **Host**: un doble de renderer recibe el objetivo en espacio del modelo (con
  el personaje trasladado y rotado), y `count = 0` cuando no hay ninguna.
- **Serialización**: ida y vuelta con dos restricciones; escena sin IK sin la
  clave; más de 4 avisa; `type` desconocido avisa.
- **Lua**: las cinco funciones, incluidos los nombres que no existen.
- **GPU**: `static_assert` del push de 24 bytes en los dos backends; el HLSL
  generado tiene `ikBlock` en su registro; runtime Debug con `rr10.scene` en
  los dos backends (Vulkan con syncval limpio, D3D12 solo `id=1328`).

Verificación manual del usuario en Vulkan y D3D12: la cabeza sigue a un objeto
al moverlo, el peso la desvanece, el ángulo máximo la frena; una mano o un pie
llega a su objetivo y el codo o la rodilla apunta donde dice el pole; un
personaje sin IK se ve igual que antes.

## Fuera de alcance

IK de más de dos huesos (FABRIK/CCD), rotación del extremo hacia la rotación
del objetivo, foot placement contra el suelo por raycast, IK por capa,
restricciones de ángulo por articulación, y el resto de la fila 15 (C9, C10,
C14).
