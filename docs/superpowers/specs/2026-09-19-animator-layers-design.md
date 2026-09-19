# Capas del Animator con máscara y peso (fila 14b, C4)

Segunda mitad de la fila 14 de `docs/animation-audit.md` (la primera, 14a, es
el blend 2D). Base: la pose por muestras ponderadas (fila 13) y el blend 2D
(14a).

## Qué se construye

Capas al estilo de Unity: cada capa es una máquina de estados completa con su
peso, su modo (override o additive) y una máscara de huesos. La capa 0 es el
grafo de hoy. Los parámetros son del componente y los ven todas las capas.

Decisiones tomadas:

- Capa = máquina de estados propia (no un estado suelto controlado por Lua).
- Máscara = sí/no por hueso, guardada en la capa como lista de nombres.
- Referencia de additive = primer fotograma del propio clip (tiempo 0).

## 1. Modelo de datos (`AnimatorComponent`)

```cpp
enum class LayerMode { Override, Additive };
struct Layer
{
    std::string              name;
    std::vector<State>       states;
    std::vector<Transition>  transitions;
    int                      entryState = -1;
    float                    weight     = 1.0f;
    LayerMode                mode       = LayerMode::Override;
    std::vector<std::string> maskBones;      // vacía = todo el cuerpo
    // --- ejecución (no se serializa) ---
    // estado actual, animTime, finished, stateTicks, prevStateTicks,
    // prevState, prevAnimTime, blendElapsed, blendDuration, frozenFade,
    // freezePending, y la máscara resuelta (uint8_t por hueso).
};
static constexpr int kMaxLayers = 8;
```

- Todo el estado que hoy vive suelto en la clase (`m_states`,
  `m_transitions`, `m_entryState`, `m_currentState`, relojes, fade,
  congelación) pasa a `m_layers[0]`. Parámetros, triggers, `m_speed`,
  eventos disparados y muestras de root motion siguen en el componente.
- La API por capa gana `int layer = 0` como último argumento:
  `states`, `statesMutable`, `transitions`, `transitionsMutable`, `addState`,
  `addTransition`, `setEntryState`, `entryState`, `currentState`,
  `currentStateName`, `previousState`, `animTime`, `normalizedTime`,
  `blendWeight`, `blending`, `fading`, `play`, `crossFade`,
  `stateBlends*`, `stateBlendSamples`. Sin argumento es la capa 0, así que
  el código y los tests existentes no cambian.
- Gestión: `layerCount()`, `layer(i)` / `layerMutable(i)`,
  `addLayer(name)` (devuelve el índice; falla por encima de `kMaxLayers`),
  `removeLayer(i)` y `moveLayer(from, to)` (nunca sobre la capa 0),
  `setLayerWeight(i, w)` / `layerWeight(i)` (acotado a [0, 1]).
- La capa 0 es siempre Override con peso 1 y sin máscara: los setters la
  ignoran para ella.
- `bindClips` / `rebindClips` resuelven los clips de todas las capas y la
  máscara de cada una contra `skeleton.names`: un nombre que no existe avisa
  (como un clip que no existe) y no marca nada. Máscara vacía = todos los
  huesos.

## 2. Evaluación en CPU

- `update` recorre las capas en orden. Cada una evalúa sus transiciones
  (con los parámetros compartidos), avanza sus relojes, sus fades y su
  congelación, exactamente como hoy la única capa. Los triggers se consumen
  al final del update, después de todas las capas: un trigger puede disparar
  transiciones en varias capas en el mismo frame.
- Eventos: los disparan todas las capas con peso > 0, en orden de capa.
- Root motion: solo la capa 0.
- `AnimationPose` pasa a:

```cpp
static constexpr int kMaxPoseSamplesPerLayer = 6;
struct PoseSample { int clip; float time; float weight; int layer; };
struct PoseLayer
{
    float    weight = 1.0f;
    uint32_t mode   = 0;                     // 0 override, 1 additive
    float    frozenWeight = 0.0f;
    bool     freezeNow    = false;
    const std::vector<uint8_t>* mask = nullptr;   // null = todo el cuerpo; lo posee el Animator
};
struct AnimationPose
{
    PoseSample samples[kMaxLayers * kMaxPoseSamplesPerLayer] = {};
    int        count = 0;
    PoseLayer  layers[kMaxLayers] = {};
    int        layerCount = 1;
    uint32_t   rootMotionMode = 0;
};
```

  `frozenWeight` y `freezeNow` salen de la raíz de la pose y pasan a
  `layers[i]`; los tests de la fila 13 pasan a mirar `layers[0]`. Las capas
  con peso 0 no emiten muestras (siguen contando en `layerCount` con
  `weight = 0`, para que el índice de capa sea estable).
  `clearFreezeRequest()` apaga la petición de todas las capas. El puntero de
  máscara solo vale durante la llamada a `setAnimationPose`: el backend lo
  copia.

## 3. GPU

- Un **bloque de pose** por personaje en un buffer `HOST_VISIBLE` con una
  copia por frame en vuelo (`N × tamBloque`); la CPU escribe la copia del
  frame actual en `setAnimationPose` y el shader recibe su offset por push
  constant. Nada de descriptores nuevos por frame. Layout std430, en uints
  (los floats con `floatBitsToUint`):

```glsl
layout(set=0, binding=10, std430) readonly buffer PoseBlock { uint data[]; } poseBlock;
// en offset: [0] layerCount, [1] sampleCount, [2..3] pad
//            capas   [4 + 4*L]   : weight, mode, frozenWeight, hasMask
//            muestras [36 + 4*k] : clipBase, time, weight, layer   (k < 48)
//            máscaras [228 + L*boneCount + bone] : 0/1
```

- El push constant de los tres `.comp` queda en `boneCount`, `vertexCount`,
  `rootMotionMode`, `poseBlockOffset` (16 bytes); `SkinningPass::Push` y
  `ComputePush` lo espejan con `static_assert(... == 16)`.
- `poseTrs` y `frozenTrs` pasan a `kMaxLayers × boneCount × 3` vec4: cada
  capa escribe y congela su propia pose (la de una capa additive es su
  delta). La copia de congelación se hace por capa (región `L`).
- `bone_eval`, por hueso:
  1. Para cada capa L: suma ponderada de sus muestras (+ su congelada) como
     hoy. En additive, cada muestra aporta su delta respecto a su clip en
     tiempo 0: posición `p(t) − p(0)`, rotación `q(t)·q(0)⁻¹` (signo
     alineado a la identidad), escala `s(t) / s(0)` (componente con
     `s(0) = 0` → 1). Hueso sin canal en una muestra additive: delta
     identidad.
  2. Resultado = capa 0. Para L ≥ 1, `m = weight_L × mask_L[hueso]`:
     - override: `pos = mix(pos, pos_L, m)`, `rot = nlerp(rot, rot_L, m)`
       (signo alineado), `scale = mix(scale, scale_L, m)`;
     - additive: `pos += m·Δp`, `rot = normalize(nlerp(identidad, Δq, m) · rot)`,
       `scale *= mix(1, Δs, m)`.
  3. El bloqueo de raíz (`rootMotionMode`) sobre el resultado final.
  4. Hueso "bind" (ninguna muestra de la capa 0 lo anima): la capa 0 aporta
     su bindLocal como TRS, y las capas se aplican encima igual.
- Un personaje sin Animator (`hasPose = false`) sigue igual: una capa, una
  muestra.

## 4. Editor (`AnimatorPanel`)

- Barra de capas encima del grafo: una fila por capa (seleccionable),
  botones **+**, **−** (no en la 0), **↑ / ↓** (no mueven la 0 ni ponen otra
  en su sitio) y renombrar con doble clic. El grafo mostrado es el de la capa
  seleccionada; los IDs de nodo siguen siendo únicos en el componente
  (`editorId` global).
- Propiedades de la capa seleccionada (no en la 0): slider **Weight**,
  combo **Override / Additive** y botón **Mask** que abre un popup con el
  árbol del esqueleto: clic en la casilla de un hueso marca o desmarca toda
  su rama; Ctrl+clic solo ese hueso. Un botón **Todo el cuerpo** vacía la
  máscara.
- Todo pasa por el undo del grafo (la clave es la serialización, que ya
  incluye las capas). `PushID` por sección.

## 5. Serialización

- Las claves de hoy son la capa 0, sin cambios.
- Solo con más de una capa: `"layers": [ { "name", "weight", "mode"
  ("override" | "additive"), "mask": [nombres] (omitida si vacía),
  "states", "transitions", "entry" }, ... ]` con las capas 1..N, y estados y
  transiciones con el mismo formato que la capa 0.
- Una escena de una capa se guarda idéntica a hoy, byte a byte.
- Más de `kMaxLayers` en el fichero: se cargan las primeras y se avisa.

## 6. Lua

- Nuevos en el Animator: `SetLayerWeight(layer, w)`, `GetLayerWeight(layer)`,
  `GetLayerCount()`.
- `Play(state, layer)`, `CrossFade(state, seconds, layer)`,
  `GetState(layer)`, `IsBlending(layer)`, `GetNormalizedTime(layer)`: el
  argumento `layer` es opcional (índice, 0 por defecto); fuera de rango no
  hace nada y los getters devuelven lo mismo que sin estado.
- `LuaApiReference.cpp` y la sección `## Lua Scripting` del README.

## 7. Pruebas

En `engine/tests/animator_tests.cpp` (y los de Lua donde estén los del
Animator), TDD, sabotajes de uno en uno:

- Refactor sin cambios: la suite existente entera en verde tras mover el
  estado a `m_layers[0]`, antes de añadir nada.
- Referencia de CPU de la combinación (`evalLayeredTrs`) sobre la de la
  fila 13, con el criterio de la fila 14: capa 1 override, peso 1, máscara de
  brazo → los huesos fuera de la máscara son idénticos a la capa 0 y los de
  dentro idénticos a la capa 1; peso 0 → la capa 0 exacta; peso 0.5 → a medio
  camino solo dentro de la máscara.
- Additive: en t = 0 el resultado es la capa 0 exacta; en t > 0 suma el delta
  (posición y rotación comprobadas a mano en un clip de dos claves).
- Capas independientes: un trigger dispara transiciones en dos capas; un
  fade en la capa 1 no toca el reloj de la 0; congelación por capa.
- Eventos de la capa 1 con peso > 0 sí, con peso 0 no; root motion solo de la
  capa 0.
- Máscara: resolución por nombre, nombre inexistente avisa, vacía = todos.
- `pose()`: muestras etiquetadas por capa, capas de peso 0 sin muestras.
- Serialización: ida y vuelta con 3 capas; escena de una capa idéntica; más
  de 8 capas avisa.
- Lua: las funciones nuevas y el argumento opcional.
- GPU: `static_assert` de 16 bytes en los dos backends; el HLSL generado
  tiene `poseBlock` como `ByteAddressBuffer` en `t10`; runtime Debug con
  `rr10.scene` en los dos backends (Vulkan syncval limpio, D3D12 solo
  `id=1328`).

Verificación manual del usuario en Vulkan y D3D12: una capa override con
máscara de brazos anima solo los brazos, el peso la desvanece, una capa
additive suma encima, la base sigue igual, y un proyecto sin capas se ve
como antes.

## Fuera de alcance

Máscaras como asset compartido, pesos de máscara fraccionarios, sincronía
de capas (el "Sync" de Unity), IK por capa, y root motion de capas que no
sean la 0.
