# Import automático de mallas rigged: habilitar el Animator sin animaciones propias

Fecha: 2026-07-19

## Problema

Un personaje rigged suele venir repartido en varios FBX: uno con la malla y el
esqueleto en T-pose (sin animaciones) y otro por cada animación. El motor ya
soporta añadir clips desde ficheros externos (`addAnimationSource`, feature
multi-FBX), pero no se puede llegar hasta ahí: el botón *Animator* del menú
*Add Component* está deshabilitado.

La causa no es el gate. Es el import:

```cpp
// PropertiesPanel::loadMeshForSelected
auto mesh = std::make_shared<Mesh>(ModelLoader::load(path));
```

`loadMeshForSelected` llama **siempre** a `ModelLoader::load()`, que construye un
`Mesh` plano y descarta huesos y pesos. `ModelLoader::loadSkinned()` no se invoca
desde ningún punto del editor. Por tanto todo FBX importado desde la UI sale
no-skinned, tenga rig o no, y el gate `canAnimate = ctx.selected->isSkinned()`
(`PropertiesPanel.cpp:1639`) nunca se satisface.

## Objetivo

Que un FBX con huesos entre como `SkinnedMesh` aunque no traiga ninguna
animación. A partir de ahí, la feature multi-FBX existente ya cubre el resto:
crear el Animator y añadirle clips desde otros ficheros.

## Alcance

Dentro:

- Detección de rig en el import del editor y en la carga de escena.
- Registro en el renderer por la ruta skinned cuando corresponda.

Fuera:

- Animator sobre objetos sin rig. Sin pesos por vértice no hay nada que
  deformar; animar el transform del nodo es un modelo de datos y una ruta de
  evaluación distintos, o sea otra feature.
- Cambios en `AnimatorPanel`, `addAnimationSource`, la serialización de
  `animationSources` o la ruta de GPU. Todo eso ya funciona sobre un
  `SkinnedMesh`, tenga 0 clips o N.

## Decisiones

| Decisión | Elegido | Motivo |
|---|---|---|
| Estático vs skinned | Auto-detectar leyendo el fichero | El FBX ya dice la verdad; un checkbox no aporta información que el usuario tenga y el loader no. |
| Escenas ya guardadas | Ignorar el flag `"skinned"` del JSON y re-detectar | Una sola regla para import y para carga; arregla las escenas viejas sin reimportar a mano. |
| Gate del Animator | Se queda (`isSkinned()`) | Con auto-detect deja de estorbar en el caso real, y sigue tapando el caso sin sentido. |
| Cómo detectar | Probe barato + delegar a los loaders existentes | No toca `load()`/`loadSkinned()`, que funcionan. |

### Enfoques descartados

**Un solo `aiScene` compartido entre construcción estática y skinned.** Cero
parseo redundante, pero exige destripar dos funciones grandes de
`ModelLoader.cpp` — incluidas las rutas de materiales y texturas embebidas —
para ahorrar milisegundos en una operación de import. Riesgo desproporcionado.

**Cargar siempre con `loadSkinned()` y degradar a `Mesh` si el esqueleto sale
vacío.** Construye vértices de 112 B para tirarlos, y `loadSkinned` tiene su
propio `throw` y sus propios flags de postproceso: ficheros que hoy `load()`
acepta podrían empezar a fallar. Regresión sin contrapartida.

## Diseño

### 1. `ModelLoader::hasBones` y `ModelLoader::loadAuto`

```cpp
// Decide estático vs skinned mirando el fichero, no al llamante. Un FBX con
// huesos entra siempre como SkinnedMesh: es lo que habilita el Animator, aunque
// el fichero no traiga ni una animación.
static std::shared_ptr<Mesh> loadAuto(const std::string& path);

// true si algún aiMesh declara huesos. No lanza: un fichero ilegible devuelve
// false y deja que el loader real dé el error de verdad, con su mensaje.
static bool hasBones(const std::string& path);
```

`hasBones` abre el fichero con los flags mínimos que ya usa
`loadAnimationClips` (sin `aiProcess_Triangulate`, `aiProcess_GenNormals` ni
`aiProcess_CalcTangentSpace`: aquí no se construye geometría) y recorre
`scene->mMeshes[i]->mNumBones`.

`loadAuto` delega:

```cpp
if (hasBones(path))
    return std::make_shared<SkinnedMesh>(loadSkinned(path));  // convierte solo
return std::make_shared<Mesh>(load(path));
```

Devuelve `shared_ptr<Mesh>` porque es exactamente lo que `GameObject::setMesh`
consume, y `GameObject::isSkinned()` ya recupera el tipo real por
`dynamic_cast`. Las excepciones de `load`/`loadSkinned` se propagan sin tocar:
los llamantes ya tienen su `try/catch`.

### 2. Import del editor

`PropertiesPanel::loadMeshForSelected` pasa a `loadAuto`, y el registro en el
renderer se bifurca según el tipo que salga:

```cpp
auto mesh = ModelLoader::loadAuto(path);
ctx.selected->setMesh(mesh);
if (ctx.selected->isSkinned())
    ctx.selected->skinnedRenderIndex = ctx.renderer->addSkinnedMesh(*ctx.selected->getSkinnedMesh());
else
    ctx.selected->staticRenderIndex  = ctx.renderer->addStaticMesh(*mesh);
```

El `setMesh` va antes del registro para poder preguntar por `isSkinned()` sin
duplicar el `dynamic_cast`. Es el mismo par de líneas que ya existe en
`sandbox/src/main.cpp:156-157`.

El mensaje del log distingue los dos casos ("Componente Mesh añadido" vs
"Componente Skinned Mesh añadido"): sin eso, el usuario no tiene forma de saber
por qué el botón *Animator* sigue gris con un FBX que él creía rigged.

### 3. Carga de escena

En `Scene.cpp:438`, la condición de la rama skinned:

```cpp
// antes
if (skinned && !sourcePath.empty())
// después
if (!sourcePath.empty() && ModelLoader::hasBones(sourcePath))
```

El campo `"skinned"` se sigue **escribiendo** al guardar (`Scene.cpp:241`): no
rompe ficheros ni herramientas externas, y sirve de dato informativo. Pero deja
de leerse. La regla es una sola, en import y en carga: manda el fichero.

Caso borde — escena guardada con `"skinned": true` cuyo FBX se reexportó luego
sin huesos. Ahora carga estática y su bloque `animationSources` se descarta. Se
emite un warning a `Scene::lastWarnings()` (el canal que lee el Log Console; un
`printf` es invisible en un build sin consola) en vez de perderlas en silencio.
Los estados del Animator que apuntaran a esos clips quedan huérfanos, cosa que
`bindClips` ya reporta por su cuenta.

### 4. Lo que no cambia

`initSkinnedRenderObject` ya contempla la malla sin animaciones
(`Renderer.cpp:1878-1893`: `kEmptyClip`, `clipCount = 1`), así que un
`SkinnedMesh` con `animationClips` vacío se sube a GPU sin caso especial nuevo.
El gate del Animator, `AnimatorPanel`, `addAnimationSource` y la serialización
de fuentes se quedan intactos.

## Tests

En `engine/tests/animator_tests.cpp`, que ya carga assets reales del repo.

Fixtures trackeados: `assets/modelAnimation.fbx` (rigged) y `assets/model.fbx`.

**Verificar antes de escribir los tests** que `assets/model.fbx` no declara
huesos. Es la suposición sobre la que descansa el caso negativo; si resulta
estar rigged, hay que añadir un FBX estático al repo como fixture.

| Test | Espera |
|---|---|
| `hasBones` sobre FBX rigged | `true` |
| `hasBones` sobre FBX estático | `false` |
| `hasBones` sobre ruta inexistente | `false`, sin lanzar |
| `loadAuto` sobre FBX rigged | puntero no nulo; `dynamic_cast<SkinnedMesh*>` no nulo; esqueleto no vacío |
| `loadAuto` sobre FBX estático | puntero no nulo; `dynamic_cast<SkinnedMesh*>` nulo |

Verificación manual en GUI, que ningún test cubre (no hay subagente con GUI):

1. Importar un FBX rigged sin animaciones (`assets/animatedCharacter/Maw J Laygo.fbx`).
2. Comprobar que *Add Component → Animator* está habilitado.
3. Añadir el Animator y, desde el panel, añadir un clip de otro fichero
   (`assets/animatedCharacter/standing idle 01.fbx`).
4. Guardar la escena, reabrirla y comprobar que la malla vuelve como skinned y
   la fuente de animación sobrevive.
