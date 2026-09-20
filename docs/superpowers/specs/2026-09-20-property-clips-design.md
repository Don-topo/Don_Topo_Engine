# Clips de propiedades: animar objetos sin esqueleto (fila 15, C14)

Segunda de las cuatro features que agrupaba la fila 15 de
`docs/animation-audit.md` (la primera fue la IK, C13). Quedan después las
curvas de clip (C9) y las sub-máquinas (C10), cada una con su spec.

## Qué se construye

El Animator deja de depender de `SkinnedMesh`: un estado puede reproducir un
**clip de propiedades** autorado en la escena, que escribe valores en el
GameObject (transform, luz, material). Con eso se animan puertas, plataformas,
ascensores, cámaras, luces que parpadean o materiales que se encienden, con el
mismo grafo de siempre: transiciones, condiciones, exit time, cross-fade,
capas, eventos y la API de Lua.

Decisiones tomadas:

- Los clips se **autoran en el editor** y viven en la escena. Ni se importan de
  un FBX (el importador resuelve canales contra un esqueleto) ni hacen falta
  para esto.
- Las propiedades animables salen de una **tabla** declarada en un sitio:
  añadir una más es una línea, y ni la UI ni la serialización crecen.
- Un estado puede llevar **los dos** clips: el de malla (si el objeto tiene
  esqueleto) y el de propiedades.

## 1. Datos (`AnimatorComponent`)

```cpp
enum class PropertyId {
    PositionX, PositionY, PositionZ,      // local, unidades de escena
    RotationX, RotationY, RotationZ,      // local, GRADOS (euler XYZ)
    ScaleX, ScaleY, ScaleZ,               // local
    LightColorR, LightColorG, LightColorB,
    LightIntensity, LightRange,
    MaterialMetallic, MaterialRoughness,
    Count
};
struct PropertyKey { float time = 0.0f; float value = 0.0f; };   // time en SEGUNDOS
struct PropertyTrack
{
    PropertyId               property = PropertyId::PositionX;
    std::vector<PropertyKey> keys;        // ordenadas por tiempo al resolver
    bool                     resolved = false;   // el objeto tiene el componente
};
struct PropertyClip
{
    std::string                name;
    float                      duration = 1.0f;   // segundos, > 0
    std::vector<PropertyTrack> tracks;
};
static constexpr int kMaxPropertyClips = 16;
```

- Cada pista anima **un escalar**. Un color o una posición son tres pistas.
  Es lo que hace la tabla trivial (un `float` por propiedad, sin variantes de
  tipo) y lo que permite animar solo la Y de una puerta sin tocar X y Z.
- El clip de propiedades vive en el componente:
  `propertyClips()` / `propertyClipsMutable()`, `addPropertyClip`,
  `removePropertyClip(int)`, y `State` gana `std::string propertyClipName` con
  su `int propertyClipIndex = -1` resuelto en `bindClips`.
- Duración del estado: si el estado no tiene clip de malla resuelto y sí clip
  de propiedades, `State::duration` y `ticksPerSecond` salen de él
  (`duration = clip.duration * ticksPerSecond` con `ticksPerSecond = 30`, para
  que el reloj del Animator —que cuenta en ticks— no necesite un caso
  especial). Con los dos, manda el de malla y el de propiedades se muestrea por
  la **fase** del estado, como los clips de un blend.

## 2. Tabla de propiedades

Una tabla estática, en `PropertyTracks.h`:

```cpp
struct PropertyDesc
{
    const char* name;                              // "position.x", "light.intensity", ...
    float (*get)(const GameObject&);               // valor actual
    void  (*set)(GameObject&, float);              // escribe
    bool  (*available)(const GameObject&);         // el objeto tiene el componente
};
const PropertyDesc& propertyDesc(PropertyId id);
```

- Transform: lee y escribe `localTransform` descompuesto (posición, euler en
  grados, escala). Se descompone y se recompone una vez por objeto y frame, no
  por pista.
- Luz: `LightComponent` (color RGB, intensidad, rango).
- Material: `metallic` y `roughness` del material del objeto.
- `available` en false: la pista queda `resolved = false`, se avisa una vez al
  resolver (`bindClips`) y no se escribe nada.

## 3. Evaluación (`PropertyTracks`)

Módulo aparte, sin escena ni ImGui, probado solo:

```cpp
// Valor de una pista en t (segundos): lineal entre keys; fuera del rango, la
// key del extremo. Sin keys, el valor actual del objeto.
float samplePropertyTrack(const PropertyTrack& track, float t, float actual);

// Lo que suena este frame: las muestras del Animator {clip, tiempo, peso} ya
// resueltas a clips de propiedades (mismo tope que las de la pose).
struct PropertySample { const PropertyClip* clip; float time; float weight; };
// Acumula el valor final por propiedad y lo escribe en el GameObject.
void applyPropertySamples(GameObject& go, const PropertySample* muestras, int n);
```

- **Mezcla**: suma ponderada por peso de las pistas que animan la misma
  propiedad. Las rotaciones se mezclan como ángulos por el **camino corto**
  (la diferencia se normaliza a [−180, 180] antes de ponderar), para que 350°
  y 10° den 0° y no 180°.
- Una propiedad que **ninguna** muestra anima no se toca: el valor del
  GameObject se queda como está (así una pista de solo Y no pisa X y Z).
- Con capas, cada muestra trae su peso ya multiplicado por el de su capa; las
  máscaras son de huesos y **no** se aplican a las propiedades (documentado).

## 4. El Animator sin esqueleto

- `AnimatorComponent::propertySamples()` devuelve las muestras del frame
  (clip de propiedades, tiempo en segundos, peso), con la misma lógica de
  cross-fade y capas que `pose()`. El reloj del Animator cuenta en ticks, así
  que el tiempo de cada muestra sale de `animTime / ticksPerSecond` del estado
  que la aporta: los clips de propiedades se autoran en segundos y no tienen
  por qué compartir el `ticksPerSecond` del clip de malla.
- `bindClips(mesh, warnings)` pasa a tener un hermano
  `bindProperties(const GameObject&, warnings)` que resuelve
  `propertyClipName` de cada estado y el `available` de cada pista. El
  componente no guarda punteros al GameObject: se resuelve cada vez que la
  escena cambia (carga, Add/Remove de componente, undo).
- `SkinnedFrameSync.h` se parte:
  - `applyAnimatorFrame(go, dt, evaluateTransitions)`: avanza el grafo, aplica
    root motion si lo hay y **escribe las propiedades**. Vale para cualquier
    GameObject con Animator.
  - `applySkinnedFrame(go, renderer, dt, evaluateTransitions)`: lo de hoy
    (pose, IK, transform, SSR al backend), que ahora llama al primero y sale si
    el objeto no tiene índice skinned.
  - Los dos hosts (runtime y sandbox) llaman a `applySkinnedFrame` como hoy;
    dentro, un objeto sin malla skinned ya no sale sin hacer nada.
- El orden en el frame no cambia: se escribe antes de propagar los
  `worldTransform`, así que un objeto animado arrastra a sus hijos en el mismo
  frame.
- `Properties → Add → Animator` deja de exigir malla con esqueleto.

## 5. Editor

Sección **Property Clips** en la columna del panel del Animator, desplegable
como las demás y con `PushID` propio:

- Lista de clips: nombre editable, duración en segundos, **+** (hasta
  `kMaxPropertyClips`) y **−**.
- Dentro del clip seleccionado, sus pistas: combo de propiedad (los nombres de
  la tabla; en rojo si no resuelve en este objeto), **+ pista**, **−**, y la
  lista de keyframes con tiempo y valor editables, ordenada por tiempo, con
  **+ key** y **x** por fila.
- En el nodo del estado, un combo **Property clip** con los del componente y
  "(ninguno)".
- Todo pasa por el undo del grafo (la clave sale de `animatorToJson`).

## 6. Serialización

- `"propertyClips": [ { "name", "duration", "tracks": [ { "property":
  "position.y", "keys": [ { "t", "v" } ] } ] } ]`, y en cada estado
  `"propertyClip": "<nombre>"`. **Las dos claves solo se escriben si se usan**:
  una escena sin clips de propiedades se guarda idéntica, byte a byte.
- Una propiedad desconocida en el fichero avisa y se descarta la pista; más de
  `kMaxPropertyClips` avisa y carga los primeros; duración ≤ 0 se acota a 0,001
  y avisa.

## 7. Lua

Sin API nueva: los clips de propiedades se conducen con el grafo
(`Play`, `CrossFade`, parámetros), que ya está expuesto. Se documenta en el
README que el Animator ya no exige esqueleto.

## 8. Pruebas

En `engine/tests/animator_tests.cpp`, TDD y sabotajes de uno en uno:

- `samplePropertyTrack`: sin keys (devuelve el actual), una sola key, antes de
  la primera, entre dos (lineal exacta), justo en una key, después de la
  última.
- Mezcla: dos muestras al 50 % dan la media; una rotación de 350° y otra de 10°
  dan 0° y no 180°; una propiedad que nadie anima no cambia.
- Tabla: las 16 propiedades leen y escriben lo que dicen (posición, euler en
  grados, escala, color, intensidad, rango, metálico, rugosidad), y
  `available` es false sin el componente.
- Resolución: `propertyClipName` que no existe avisa y deja el estado sin clip
  de propiedades; pista con propiedad de un componente que falta avisa una vez.
- Grafo sin esqueleto: un GameObject sin malla skinned con dos estados y una
  transición por trigger mueve su `localTransform` al avanzar, y en Edit
  (`evaluateTransitions = false`) el grafo no transiciona pero el tiempo corre.
- Cross-fade entre dos clips de propiedades: el valor pasa de uno a otro sin
  saltos (muestreo en varios puntos del fade).
- Serialización: ida y vuelta con dos clips y tres pistas; escena sin clips sin
  las claves nuevas; propiedad desconocida, tope y duración ≤ 0 avisan.

Verificación manual del usuario en Vulkan y D3D12: una puerta (cubo sin
esqueleto) con dos estados y un trigger abre y cierra; una luz parpadea con un
clip; un personaje conserva su animación de malla y además le cambia el
material; una escena vieja se ve igual.

## Fuera de alcance

Editor de curvas (interpolación distinta de la lineal), pistas de propiedades
de otros componentes (audio, físicas, cámara), animar propiedades de un hijo
desde el padre, importar clips de propiedades de un FBX (C9/C10 y la opción 2
del brainstorming), y máscaras de capa sobre propiedades.
