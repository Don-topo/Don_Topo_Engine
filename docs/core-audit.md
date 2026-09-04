# Auditoría del módulo Core

Fecha: 2026-09-04. Rama: `main` (`4782396`).

Alcance leído, entero y verbatim: `engine/src/Core/**` (9 `.cpp`) y
`engine/include/DonTopo/Core/**` (12 `.h`). **21 ficheros, 6.057 LOC** medidos
con `wc -l`, no estimados.

Se ha leído fuera del alcance para entender a los consumidores —
`engine/src/Editor/{Command,ScenePanel,EditorUI,AnimatorPanel,PropertiesPanel}.cpp`,
`engine/src/Scripting/{ScriptBindings,ScriptManager}.cpp`,
`engine/src/Renderer/AsyncAssetLoader.cpp`, `engine/src/Physics/`,
`engine/tests/**`, `runtime/main.cpp`, `sandbox/src/main.cpp`— pero **ninguna
fila de aquí tiene su causa fuera del alcance**: cuando el consumidor es lo que
salva la situación, la fila lo dice.

Todo `fichero:línea` de este documento se ha leído. Todo número ("N llamantes",
"N duplicados", "N miembros") sale de un `grep` ejecutado, no de una
estimación — la auditoría anterior (`docs/renderer-audit.md`) documenta que
**todos** sus recuentos estimados salieron cortos, y aquí ha vuelto a pasar
donde se estimó a ojo antes de contar (la lambda duplicada de `nodeFromJson`
parecía "unas cuantas": son 28).

**Lo que esta auditoría NO re-litiga**, porque `docs/renderer-audit.md` ya lo
midió y lo tumbó:

- **Partir un `.cpp` grande no acelera la compilación** (H8 de aquella: el
  `Renderer` pasó de 15 a 6 ficheros y el build fue de 27 a **28** s). La
  propuesta P6 de abajo propone partir `Scene.cpp` y **no invoca el tiempo de
  compilación como beneficio**: su criterio es de acoplamiento, y es binario.
- **El overhead por-draw no es rentable** (P14 de aquella). Nada de aquí lo toca.
- **La RHI ya existe** y el split Core/Editor está hecho y mergeado.

`MAX_LIGHTS` vale hoy **64** (`include/DonTopo/Renderer/UniformBufferObject.h:30`),
ya subido por P2 de la auditoría del Renderer: `Scene::collectLights` no
aparece aquí por el tope, sino por no tener ni un test.

---

## 1. Inventario

### 1.1 Ficheros

| Fichero | Responsabilidad | LOC | Notas |
|---|---|---|---|
| `src/Core/Scene.cpp` | Serialización del `.scene`, invariantes del árbol, sync de física/audio, recolección de luces y canvas | 3241 | **80,3 % es serialización** (ver 1.2) |
| `include/DonTopo/Core/Scene.h` | API de la escena: 26 declaraciones públicas, 3 privadas | 261 | 170 de sus 261 líneas son comentario de contrato |
| `src/Core/AnimatorComponent.cpp` | Máquina de estados: reloj, cross-fade, blend por parámetro, transiciones | 521 | Lógica pura, sin GPU ni `GameObject` |
| `include/DonTopo/Core/AnimatorComponent.h` | `State`/`Transition`/`Condition`/`Parameter` + API de diseño y runtime | 308 | 4 tipos de parámetro, 5 de condición |
| `include/DonTopo/Core/GameObject.h` | Nodo del árbol: 28 miembros privados, 12 campos públicos, 29 `has*()` | 342 | **33 `#include`**; lo incluyen 24 ficheros |
| `src/Core/Input.cpp` | Fachada estática sobre teclado/ratón/mando + acciones con nombre | 269 | Estado `static`, sin instancia |
| `include/DonTopo/Core/Input.h` | Bindings de acción, códigos de eje empaquetados, histéresis | 110 | |
| `src/Core/JobSystem.cpp` | Pool de hilos genérico con cancelación e id reservable | 199 | Un único consumidor real: `AsyncAssetLoader` |
| `include/DonTopo/Core/JobSystem.h` | Contrato del pool; 56 de sus 114 líneas son comentario, casi todo la carrera de `shutdown()` | 114 | |
| `include/DonTopo/Core/LightComponent.h` | Luz: tipo, color, intensidad, alcance, cono, área. Todo con clamp | 108 | Header-only |
| `src/Core/Camera.cpp` | Cámara de vuelo del editor (WASD + mando + órbita) | 98 | **Cero referencias en tests** |
| `include/DonTopo/Core/Camera.h` | | 40 | |
| `src/Core/CameraComponent.cpp` | Proyección (persp./ortho, `*_ZO` + Y-flip) y view desde `worldTransform` | 83 | |
| `include/DonTopo/Core/CameraComponent.h` | | 61 | |
| `src/Core/Window.cpp` | Ventana GLFW, icono, arranque oculto (anti flash blanco) | 75 | |
| `include/DonTopo/Core/Window.h` | | 43 | |
| `include/DonTopo/Core/TransformDecompose.h` | `decomposeTransform`: descomponer sin quedarse con basura | 67 | Header-only; 30 líneas de comentario del bug que lo motivó |
| `include/DonTopo/Core/ReflectionProbeComponent.h` | Radio e intensidad de sonda, con clamp | 52 | Header-only |
| `src/Core/GameObject.cpp` | Constructor con id atómico, `addChild`, propagación de transforms, scripts | 42 | |
| `src/Core/Engine.cpp` + `.h` | **Vacío**: constructor y destructor sin cuerpo | 9 + 14 | Ver H21 |
| **Total** | | **6.057** | |

### 1.2 Qué hace `Scene.cpp` de verdad

Antes de opinar sobre sus 3.241 líneas, el desglose real por rangos (fronteras
verificadas: el namespace anónimo cierra en `:2633`, `namespace DonTopo` abre en
`:2635`):

| Rango | Qué es | LOC | % |
|---|---|---|---|
| 1-32 | 30 `#include` (Physics, Audio, Renderer, Files, Scripting, glm, nlohmann) | 32 | 1,0 % |
| 33-99 | 39 `using DonTopo::…` + 4 forward declarations | 67 | 2,1 % |
| 100-496 | **21 pares** `xxxToStr`/`xxxFromStr` de enums del `.scene` | 397 | 12,2 % |
| 498-644 | `animatorToJson` / `animatorFromJson` | 147 | 4,5 % |
| 646-1187 | `nodeToJson`: serializa los 28 slots de componente de un nodo | 542 | 16,7 % |
| 1189-1451 | Lectores tolerantes (`readFloat`/`readBool`/`readString`/`readArrayFloat`/`jsonToMat4`/`jsonToVec3`/`jsonToVertex`) + `proceduralMeshByName` | 263 | 8,1 % |
| 1453-2632 | `nodeFromJson`: **33 bloques `if (j.contains(...))`**, uno por componente | 1180 | 36,4 % |
| 2635-3241 | **La clase `Scene`**: 26 definiciones `Scene::` | 607 | 18,7 % |

**El namespace anónimo (33-2633) son 2.601 líneas: el 80,3 % del fichero.** La
clase que da nombre al fichero es el 18,7 %. Esto no es una opinión sobre el
tamaño: es el dato que hace falta para juzgar P6.

### 1.3 Recorridos del árbol por frame

Contado leyendo el bucle del editor (`sandbox/src/main.cpp:1016-1135`) y el del
runtime (`runtime/main.cpp:665-728`). Un "recorrido" es una pasada completa por
todos los `GameObject`:

| # | Origen | Play (editor) | Edit Mode | Runtime |
|---|---|---|---|---|
| 1 | `Scene::update` → sync de colliders (`Scene.cpp:3048`) | ✅ | — | ✅ |
| 2 | `Scene::update` → `updateWorldTransforms` (`:3114`) | ✅ | ✅ (`main.cpp:1033`) | ✅ |
| 3 | `Scene::update` → `updateAudioSpatial` (`:3118`) | ✅ | ✅ (`main.cpp:1038`) | ✅ |
| 4 | `syncReverbZones` (`:3135`) | ✅ | — | ✅ |
| 5 | `scene.traverse` del host (empuja transforms al Renderer) | ✅ | ✅ | ✅ |
| 6 | `collectLights` (`:2952`) | ✅ | ✅ | ✅ |
| 7 | `collectCanvases` (`:2897`) | ✅ | ✅ | ✅ |
| 8 | `findAudioListener` (`:2793`) | ✅ | — | ✅ |
| 9 | `findCamera` desde el Renderer (`Renderer.cpp:909`) | ✅ | ✅ | ✅ |
| | **Total** | **9** | **6** | **9** |

El recuento es correcto y **el coste está medido** (ver P4): los nueve juntos
son **238 µs con 5.000 nodos** en Release, el **1,4 % de un frame a 60 fps**.
Se conserva la tabla porque describe la forma del bucle, no porque haya nada
que arreglar en ella.

---

## 2. Hallazgos

22 filas: **2 Alta, 10 Media, 10 Baja**. La pasada de auditoría las dejó todas
`ABIERTO`. Cerradas después: las **dos Altas** (H1 por P1, H14 por P3) y
**H22** (por P7); **H2 queda reducido** — sus 7 accesos crudos ya no existen,
pero siguen sus 57 `.value(...)`. Cada fila lo dice en su celda de estado.
**H12 y H13 se descartaron por medición** (P4 y P5), sin tocar producción, y
**H3 se cerró** por P8. **Quedan 15 abiertas**, todas Media o Baja.

### 2.1 Identidad de los objetos

| ID | Severidad · Estado | fichero:línea | Qué pasa | Por qué importa / qué se encontró de verdad | Talla |
|---|---|---|---|---|---|
| H1 | **Alta** · **CERRADO** | `src/Core/Scene.cpp:1470-1471` y `src/Core/GameObject.cpp:8-10` | `nodeFromJson` reusa el `id` que trae el fichero, pero **nadie adelanta el contador global**. `grep` sobre todo el repo: `Scene.cpp:1471` es la ÚNICA escritura a `GameObject::id` fuera del constructor, y el contador `s_nextId` solo lo toca `GameObject.cpp:10`. | Tras cargar una escena, `addGameObject` puede repartir un id **que ya existe en el árbol**. Repro en 4 pasos: sesión A crea un objeto (id 2), lo borra, crea otro (id 3) y guarda; sesión B fresca arranca con `s_nextId`=2, carga (un `addChild` consume el 2, y `:1471` lo pisa con el 3), y el siguiente objeto que el usuario cree estrena el **3**. `findById` no corta el recorrido y devuelve **el último** (`:2765-2770`), y `Command.cpp` lo llama en **31 sitios** para resolver su objetivo en cada `execute()`/`undo()`: el Undo escribe en el objeto equivocado, en silencio. Es **el mismo fallo** que el código ya reconoce y protege en el camino del clon (`:2704-2716`, tests `test_clone_gets_fresh_id` y `test_clone_subtree_gets_fresh_ids` en `camera_tests.cpp:441-479`); el camino de carga no tenía ni guarda ni test. **Cerrado por P1.** | S |
| H17 | Baja · ABIERTO | `src/Core/Scene.cpp:2765-2770` | `findById` recorre el árbol **entero** sin cortar y se queda con la última coincidencia. | Con ids únicos da igual y el coste es O(n) por llamada — pero se llama 31 veces desde `Command.cpp` más una desde `AnimatorPanel.cpp:869` y otra desde `EditorUI.cpp:1093`. Lo que importa no es la velocidad: es que "el último gana" es lo que convierte H1 en escrituras al objeto equivocado en vez de en un fallo ruidoso. Cortar en la primera no arregla H1 (seguiría resolviendo al nodo cargado en vez de al nuevo), pero un `assert` de unicidad aquí sí lo delataría. | S |

### 2.2 Carga de escena: qué sobrevive a un fichero corrupto

| ID | Severidad · Estado | fichero:línea | Qué pasa | Por qué importa / qué se encontró de verdad | Talla |
|---|---|---|---|---|---|
| H2 | Media · **REDUCIDO** (los 7 crudos, cerrados por P2; siguen los 57 `.value`) | `src/Core/Scene.cpp`, los 7 sitios ya no existen | Quedan **7 accesos JSON crudos** (`.at(...)` / `get<T>` sin guarda de tipo) y **57 `.value(...)`** contados con `grep`. Cualquiera lanza `json::exception` ante un `null` o un tipo que no toca. | La excepción sube hasta el catch de `fromJson` (`:3203-3206`) y **se pierde la carga de la escena ENTERA**, sin decir qué campo. Es exactamente lo que el comentario grande de `:1189-1225` dice haber arreglado — pero solo para los campos que pasaron a `readFloat`/`readBool`/`readString`. Los que quedan son de la misma familia: `j.at("children")` y `childJson.at("name")` los escribe `nodeToJson` SIEMPRE (`:1182-1184`), así que su ausencia es corrupción, no back-compat, y merece un aviso, no perder el fichero. **No he encontrado hoy un camino que escriba `null` en ninguno de los 7** (Lua no expone `ssrIntensity`: verificado con `grep` sobre `ScriptBindings.cpp`), así que la fila es Media y no Alta: es una guarda incompleta, no un fallo reproducible. **CORRECCIÓN al hacer P2**: lo de "pierde la escena entera" valía para **5 de los 7**, no para los 7 — comprobado test a test. Los otros dos (`clips` e `indices`) caen DENTRO del try/catch de la carga de malla (`:1683`), que atrapaba la excepción mucho antes del catch de `fromJson`. Su daño era otro y peor de diagnosticar: el nodo se cargaba **sin malla** —y en el caso de `clips`, un personaje **sin rig, sin Animator y sin esqueleto**— con un único aviso que decía "no se pudo cargar la malla", sin nombrar el campo. Un fallo silencioso disfrazado de otro. Es el patrón que avisa la cabecera de `docs/renderer-audit.md`: al ir a hacer la fila, el motivo declarado resultó ser otro. **Quedan abiertos los 57 `.value(...)`**, que P2 no tocaba. | M |
| H22 | Media · **CERRADO** | `src/Core/Scene.cpp:1875-1881, 1918-1924` y 12 repeticiones más | Dentro de `nodeFromJson` hay **14 copias literales** de la lambda `readBool` y **14 de `readStr`** (contadas con `grep -c`), una por bloque de componente de UI. | Estimé "unas cuantas" antes de contar; son **28**. Y no son inocuas: la lambda `readStr` devuelve `std::string()` ante un valor corrupto **sin avisar**, mientras que la `readString` del namespace (`:1284-1304`) sí avisa por el mismo caso. O sea: un float corrupto en un componente de UI se reporta al Log y un string corrupto en el MISMO componente se traga en silencio, según por cuál de las dos rutas casi-idénticas pase. La duplicación no es el problema; la divergencia de comportamiento que esconde, sí. **Cerrado por P7.** | M |
| H8 | Baja · ABIERTO | `src/Core/Scene.cpp:613-614, 642` | `animatorFromJson` no valida `entryState` ni `from`/`to` de las transiciones. `setEntryState` (`AnimatorComponent.cpp:61-66`) valida y **retorna sin hacer nada** si el índice está fuera de rango; `update` (`:478`) descarta una transición con `toState` inválido. | Un grafo guardado con `entryState: 3` y solo 2 estados (FBX reexportado, edición a mano) entra en el estado 0 sin decir nada, y una transición con `from` inválido queda muerta y muda. Todo lo demás en este fichero avisa de este tipo de anomalía: aquí no hay canal, aunque `animatorFromJson` ya recibe `warnings` y lo usa para los `readFloat`. | S |

### 2.3 Contratos, invariantes y obligaciones del llamante

| ID | Severidad · Estado | fichero:línea | Qué pasa | Por qué importa / qué se encontró de verdad | Talla |
|---|---|---|---|---|---|
| H3 | Media · **CERRADO** | `include/DonTopo/Core/Scene.h:35` | `removeGameObject` es la ÚNICA de las cinco funciones que mutan el árbol sin una línea de contrato en el header. `reparent` (`:37-57`), `insertFromJson` (`:139-147`) y `cloneGameObject` (`:149-155`) documentan cada una la suya, incluida "el caller debe registrar los meshes en GPU". | Los **3** llamantes externos (`Command.cpp:49-50`, `ScenePanel.cpp:215+234` vía `ctx.onDelete` → `EditorUI.cpp:205`, `ScriptManager.cpp:546-547` vía `m_onDestroying`) llaman a `Renderer::removeGameObject` ANTES, y hoy los tres lo hacen bien. Pero lo hacen por costumbre, no por contrato: el header no lo pide. Es el patrón "acuérdate de llamar también a X", que en este repo ya ha fallado varias veces; y el hueco liberado en el Renderer es justo el recurso que `slot_pool_tests.cpp:51` existe para proteger. **Al ir a cerrarlo salió que era peor de lo que decía la fila**: no eran tres llamantes que se acordaban, eran **tres implementaciones distintas del mismo aviso** — `EditorUI::onDelete`, `ScriptManager::onDestroying` y la llamada a pelo del comando de Undo, el único sin hook propio. **Cerrado por P8.** | S |
| H7 | Baja · ABIERTO | `include/DonTopo/Core/Scene.h:65-71` vs `src/Core/Scene.cpp:3004-3044` | `Scene.h` declara `findCamera` "única fuente de verdad del invariante «como mucho una cámara por escena»". `fromJson` lo impone con `pruneExtraCameras` (`:3226`) y `cloneGameObject` con un descarte explícito (`:2754-2759`, con test). **`insertFromJson` no impone nada.** | Intenté alcanzar el estado de dos cámaras por esta vía y **no se puede hoy**: el stack de Undo es LIFO, así que para deshacer el borrado de la cámara A hay que deshacer antes el alta de la B. La fila se queda porque lo que protege el invariante es el orden de una estructura que vive fuera de `Scene`, mientras el header afirma que la fuente de verdad está dentro. Un cambio en el stack de Undo (saltar entradas, deshacer selectivo) lo rompería sin tocar `Scene`. | S |
| H6 | Baja · ABIERTO | `src/Core/Scene.cpp:3146-3156` | `Scene::shutdown` pone a nulo los 4 colliders, el `AudioClip` y los scripts. **No toca** `Rigidbody`, `Animator`, `ReverbZone` ni `AudioListener` (`GameObject.h:318, 321-323`). | El `Rigidbody` guarda el actor de PhysX como `void* m_actor` (`Rigidbody.h:45-46`) y el actor lo libera `~Collider` (`Collider.h:73` + el comentario de `:155`): tras `shutdown()` ese puntero queda colgando. Hoy no se usa porque el único llamante (`fromJson:3208`) destruye acto seguido el árbol viejo con el move-assignment de `:3209`, así que ningún `Rigidbody` superviviente lo lee. Fila Baja por eso — pero la función se llama `shutdown`, es pública, y su comentario en `Scene.h:195` no dice ni qué limpia ni qué deja. Las zonas de reverb sobreviven igual y solo se recogen en el siguiente `syncReverbZones` (`:3143`), que en Edit Mode **no se llama** (`main.cpp:1023-1039`). | S |
| H20 | Baja · ABIERTO | `src/Core/Scene.cpp:3046` y `:3146` | `Scene::update(float, PhysicsManager&)` y `Scene::shutdown(PhysicsManager&, AudioManager&)` **no usan sus parámetros** — los tres van comentados en la firma. | No es cosmética: obliga a todo llamante a tener un `PhysicsManager` vivo para llamarlos. `engine/tests/camera_tests.cpp:8` documenta que los tests montan uno solo para satisfacer la firma, y con PhysX **solo cabe una `PxFoundation` por proceso**, así que ese `PhysicsManager` hay que compartirlo entre todos los tests del binario. Un parámetro muerto ha impuesto una restricción real al arnés de pruebas. | S |
| H21 | Baja · ABIERTO | `src/Core/Engine.cpp:5-7` y `include/DonTopo/Core/Engine.h:5-12` | La clase `Engine` es un constructor y un destructor **vacíos**. El header promete `throws std::runtime_error on init failure` (`Engine.h:7`) y no lanza nunca. | 23 LOC que no hacen nada y un comentario que describe un comportamiento inexistente. Nadie la instancia en `runtime/main.cpp` ni en `sandbox/src/main.cpp`. Borrarla o implementarla; lo que no puede quedarse es la promesa falsa en el header. | S |

### 2.4 Componentes: Animator, Input, Window

| ID | Severidad · Estado | fichero:línea | Qué pasa | Por qué importa / qué se encontró de verdad | Talla |
|---|---|---|---|---|---|
| H4 | Media · ABIERTO | `src/Core/AnimatorComponent.cpp:52` y `:65` | `removeState` y `setEntryState` terminan en `reset()`, que borra `m_currentState` y **todos** los bool/trigger/int/float del usuario (`:280-296`). | `AnimatorComponent.h:158-163` documenta ese peligro palabra por palabra —y por eso existe `rebindClips`, que es `bindClips` sin el `reset()`— pero solo para el camino de las fuentes de animación. Los dos vecinos se quedaron con el `reset()`. Y son alcanzables en Play: `AnimatorPanel.cpp:435` y `:480` los llaman y `grep` confirma que **ese fichero no consulta `isPlaying` ni una vez**. Tocar el grafo a mitad de partida resetea la máquina de estados y los parámetros que el script Lua venía escribiendo. | M |
| H5 | Media · ABIERTO | `src/Core/Input.cpp:40, 187, 190, 206-211` | `takeActionDiagnostics()` **siempre devuelve una lista vacía**: `grep` sobre todo el repo dice que `s_actionDiagnostics` solo se declara (`Input.h:107`), se define (`Input.cpp:40`) y se vacía (`:209`). **Nadie escribe en ella.** | `Input.h:82-84` promete "avisos acumulados al cargar (bindings de mando ignorados)", y hay un consumidor real que los vuelca al Log (`ScriptBindings.cpp:501`). Los dos sitios que descartan un binding —código de eje fuera de rango (`:187`) y dispositivo desconocido (`:190`)— lo hacen con un `continue` mudo. Resultado: una acción que no dispara nunca porque su binding se descartó al cargar es indistinguible de una acción mal configurada, y el canal que existe para explicarlo está desconectado desde el primer día. | S |
| H10 | Baja · ABIERTO | `src/Core/Input.cpp:227` vs `:243-244` y `:260-261` | `isActionDown` resuelve un binding de ratón con `isMouseButtonDown(b.code)`, que consulta GLFW directamente **sin acotar el código** (`:89-92`). Sus dos hermanas sí lo acotan contra `GLFW_MOUSE_BUTTON_LAST` antes de indexar `s_mCurr`. | No hay acceso fuera de rango a memoria propia (GLFW valida y devuelve `RELEASE`), pero las tres funciones de la misma familia se comportan distinto ante el mismo `input_actions.json` corrupto: dos lo descartan y una se lo pasa a GLFW, que además emite un error de GLFW por consulta y por frame. | S |
| H9 | Baja · ABIERTO | `src/Core/Window.cpp:59-69` | `shouldClose()` llama a `glfwWindowShouldClose(m_window)` sin comprobar nulo, a diferencia de `show()` (`:54-57`) que sí lo hace. Y `shutdown()` llama a `glfwTerminate()` (`:63`), que apaga GLFW **para todo el proceso**. | Un `Window` sin `init` (o ya cerrado) pasa `nullptr` a GLFW, que lo trata como error de programación. Y `glfwTerminate` dentro del `shutdown` de una instancia convierte a `Window` en una clase que solo puede existir una vez por proceso, cosa que ni el header ni el nombre dicen. Hoy hay exactamente una en cada host, así que no muerde. | S |
| H11 | Baja · ABIERTO | `src/Core/JobSystem.cpp:143-148` y `:103` | `cancel(id)` inserta en `m_cancelled` sin comprobar si el job existe, ya corrió o ya se descartó. El set solo se vacía en `shutdown()` (`:103`). | Un `cancel()` de un job que ya terminó deja su id dentro **para toda la vida del pool**. `AsyncAssetLoader.cpp:140` y `:380` cancelan cargas; en una sesión larga de editor con muchos Load Scene el set crece monótono. No es un bug de corrección (los ids no se reutilizan: `m_nextId` solo sube) y el coste por entrada es mínimo — es una fuga acotada por la duración de la sesión, dicha aquí para que no sorprenda. | S |

### 2.5 Rendimiento

| ID | Severidad · Estado | fichero:línea | Qué pasa | Por qué importa / qué se encontró de verdad | Talla |
|---|---|---|---|---|---|
| H14 | **Alta** · **CERRADO** | `src/Core/Scene.cpp:2737` → `:1567-1568` | `cloneGameObject` llama a `nodeFromJson` con **7 argumentos**: `loader` y `preloaded` caen a sus `nullptr` por defecto. Para un origen skinned, eso lleva la ejecución a `ModelLoader::loadSkinned(sourcePath)`: **una lectura síncrona de disco con Assimp, en pleno bucle de Play**. | El único llamante de `cloneGameObject` es `Scene.Instantiate` de Lua (`ScriptBindings.cpp:3870`), que por definición corre en Play. Spawnear un personaje animado desde un script reparsea su FBX entero desde disco, cada vez. La ironía está a 4 líneas: `:2732-2734` siembra una cache **solo para `hasBones`** —precisamente para no sondear el FBX— y acto seguido el camino skinned lo lee entero igual. El propio comentario de `:2726-2731` explica que leer disco al clonar es peligroso porque un reexport a media partida devuelve un tipo de malla distinto; el argumento vale igual para la malla en sí. Ya **medido** (ver P3): **24,5 ms por clon** en Release, o sea frame y medio a 60 fps por cada spawn. **Cerrado por P3.** | M |
| H12 | Media · **DESCARTADO POR MEDICIÓN** | `src/Core/Scene.cpp:3048, 3114, 3118, 3135, 2952, 2897, 2793` | **9 recorridos completos del árbol por frame** en Play y en el runtime, 6 en Edit Mode. La tabla 1.3 los enumera uno a uno con su línea. | Cada uno vuelve a pagar el mismo salto de punteros por un árbol de `unique_ptr` con nodos de ~700 bytes (H19). Cuatro de los nueve (`updateWorldTransforms`, `updateAudioSpatial`, `collectLights`, `findAudioListener`) podrían compartir una sola pasada sin cambiar ningún resultado, porque los cuatro leen `worldTransform` después de propagarlo. `findCamera` desde el Renderer (`Renderer.cpp:909`) es un recorrido entero para encontrar **un** nodo, por frame. **Ya medido** (ver P4): los 9 recorridos juntos son **238 µs con 5.000 nodos** en Release, el **1,4 % de un frame a 60 fps** — y lo que una fusión ahorraría de verdad son **64 µs, el 0,38 %**. El recuento de 9 era correcto; lo que estaba mal era suponer que importaba. | M |
| H13 | Media · **DESCARTADO POR MEDICIÓN** | `include/DonTopo/Core/GameObject.h:97-104`, usado en `src/Core/Scene.cpp:3049` | `anyCollider()` devuelve `std::shared_ptr<Collider>` **por valor**, y además construido desde un `shared_ptr` de tipo derivado: cada llamada es un incremento y un decremento atómicos del contador. | `Scene::update` la llama **una vez por GameObject y por frame** (`:3049`), incluidos todos los nodos que no tienen collider (ahí devuelve un `shared_ptr` nulo, que sigue siendo una construcción). En una escena de N nodos son 2N operaciones atómicas por frame sin ninguna utilidad: el valor solo se usa para comprobar `!col` y llamar a tres métodos, todos dentro del mismo scope donde el `GameObject` está vivo. Devolver `Collider*` crudo es equivalente en seguridad aquí. **CORRECCIÓN, y es lo que invalida la fila**: lo de "2N operaciones atómicas por frame" es **falso**. Copiar un `shared_ptr` **nulo no toca ningún contador atómico**, y el nodo típico no tiene collider — o sea que justo el caso que la fila señalaba como desperdicio es el que ya era gratis. Y en los nodos que **sí** tienen collider, el átomo queda enterrado bajo el trabajo real de al lado (`getWorldTransform`, la comparación de 16 flotantes, `getWorldScale`). Medido con ablación: **sin diferencia** (ver P5). | S |
| H15 | Media · ABIERTO | `src/Core/Scene.cpp:685-689` y `:1666-1671` | Para una malla **procedural** (sin `sourcePath`), `nodeToJson` serializa cada vértice como un objeto JSON con 5 sub-arrays, y `nodeFromJson` lo reparsea con `jsonToVertex`. `cloneGameObject` pasa por los dos (`:2698` y `:2737`). | Una esfera por defecto son `(32+1)×(16+1) = 561` vértices (`Sphere.h:11`), o sea **8.415 números** por `Instantiate`, más los índices. Y no es solo el JSON: `jsonToVertex` (`:1423-1435`) construye **5 strings de contexto por vértice** (`contexto + ".pos"`, `+ ".color"`, …) sobre el string que el bucle de `:1669` ya construye por vértice — 6 asignaciones de heap por vértice, ~3.400 por esfera clonada, todas para un mensaje de aviso que casi nunca se emite. El camino de disco (H14) y este cubren entre los dos los dos tipos de malla que se pueden clonar. | M |
| H16 | Baja · ABIERTO | `include/DonTopo/Core/GameObject.h:278-283` | `traverse` toma `Fn fn` **por valor** y recursa con `c->traverse(fn)`, también por valor: una copia del functor por cada hijo y por cada nivel. | Con lambdas de captura pequeña es ruido; con las de `Scene::update` (`:3048`) y `collectLights` (`:2952`), que capturan por referencia, también. Se menciona porque el arreglo es de una línea (`Fn&& fn` + `std::forward`) y porque multiplica el coste de H12 por el número de nodos, no por el de recorridos. | S |

### 2.6 Arquitectura

| ID | Severidad · Estado | fichero:línea | Qué pasa | Por qué importa / qué se encontró de verdad | Talla |
|---|---|---|---|---|---|
| H18 | Media · ABIERTO | `src/Core/Scene.cpp:33-2633` vs `:2635-3241` | El namespace anónimo de serialización son **2.601 líneas, el 80,3 %** del fichero. La clase `Scene` son **607, el 18,7 %**. | El dato que importa no es el tamaño sino **quién arrastra las dependencias**: las 30 `#include` de `:1-32` (PhysicsManager, AudioManager, ModelLoader, AsyncAssetLoader, Cube/Sphere/Plane/Capsule, FileManager, ScriptComponent) las necesita casi toda la serialización y **casi ninguna la clase `Scene`**. Ver P6, que mide esto exactamente y NO invoca el tiempo de compilación (ver la cabecera: eso ya se tumbó en H8 del Renderer). | L |
| H19 | Media · ABIERTO | `include/DonTopo/Core/GameObject.h:1-34, 312-340` | **33 `#include`** (28 de ellos de componentes concretos: UI, Physics, Audio, Core) y **28 miembros privados** (27 `shared_ptr` + 1 `vector`), más 12 campos públicos y 29 `has*()`. Lo incluyen **24** ficheros; a `Scene.h` (que lo incluye) lo incluyen otros **23**. | Dos consecuencias distintas, y solo una es de compilación. (a) Tocar cualquier componente de UI —`SliderComponent.h`, por ejemplo— recompila todo lo que ve `GameObject.h`. (b) La que no depende del build: `sizeof(GameObject)` son ~700 bytes de los que **27 punteros están a nulo en el nodo típico**, y los 9 recorridos por frame de H12 los pasean enteros por la cache. Un mapa de componentes arreglaría (b) y (a) a la vez, pero es una L que toca los 24 ficheros: aquí solo se deja medido, no propuesto. | L |

---

## 3. Propuestas

Cada una lleva su plan de medición o su criterio binario. Las que no tienen
medición hecha van marcadas **sin medir** en la propia fila, tal como pide el
encargo.

| ID | Severidad · Estado | fichero:línea | Qué propone | Plan de medición / criterio binario | Talla |
|---|---|---|---|---|---|
| P1 | **Alta** · **HECHO** (2026-09-04) | `src/Core/GameObject.cpp:12-28` + `src/Core/Scene.cpp:1470-1478` | Cierra H1. `GameObject::reserveIdAtLeast(uint64_t)` empuja el contador con un CAS en bucle (`std::atomic` no tiene `fetch_max`, y leer-comparar-escribir por separado dejaría que dos hilos se pisaran el avance); `nodeFromJson` la llama justo después de asignar el id del fichero. | **Criterio binario cumplido.** Test `test_load_advances_id_counter` (`camera_tests.cpp:509-559`): sonda el contador con un `GameObject` recién construido, fabrica el fichero con un id 20 posiciones por delante, carga y da 30 altas nuevas. **Visto en rojo antes de escribir el arreglo**, y por las dos razones correctas: `adjacent_find` encontraba el id repetido (línea 553) y `findById` devolvía el objeto NUEVO en vez del cargado (línea 557). En verde después. Suite completa: **25/25 ejecutables** (ojo: desde la raíz del repo — 5 de ellos fallan desde `build-ninja/engine/tests` por no resolver `assets/`, y eso no tiene nada que ver con el cambio). La sonda es lo que hace el test determinista sin depender del orden de ejecución de los demás tests del binario, que comparten el contador. | S |
| P2 | Media · **HECHO** (2026-09-04) | `src/Core/Scene.cpp` (los 7 sitios) | Cierra la mitad cruda de H2. Los 7 accesos pasan por los lectores tolerantes del propio fichero, con `required=true` donde `nodeToJson` escribe siempre. Criterios por sitio: `id` corrupto → el nodo conserva el que le puso el constructor (inventar un 0 para todos los rotos los haría chocar entre ellos); `indices` y `clips` → se descarta la lista **entera**, porque media geometría y —sobre todo— una lista de nombres a medias son peores que ninguna (los nombres se aplican POSICIONALMENTE: saltarse el elemento malo renombra los clips siguientes); `useGravity` legacy → se ignora y el collider queda static; script sin `name` → se descarta ese componente, no el objeto; `children`/`name` ausentes → el nodo se carga sin hijos o sin nombre. | **Criterio binario cumplido**: 7 tests, uno por campo (`camera_tests.cpp:423-535` y `animator_tests.cpp:1088`), cada uno exigiendo las tres cosas — la escena carga, el nodo sano llega entero y el aviso **nombra el campo**. Sin la tercera, el arreglo sería tragárselo en silencio, que es peor que fallar. **Los 7 vistos en rojo primero**, y ahí salió la corrección de H2: 5 fallaban por `fromJson == false` y 2 por perder la malla en silencio. Suite 25/25 en Debug y Release. | M |
| P3 | **Alta** · **HECHO** (2026-09-04) | `src/Core/Scene.cpp:2751-2763` | Cierra H14. `cloneGameObject` siembra una `PreloadedMeshCache` con las mallas que el origen **ya tiene en memoria** y se la pasa a `nodeFromJson`; el camino de copia profunda ya existía (`:1560-1566`). Se siembra recorriendo el **subárbol**, no solo la raíz: la cache de `hasBones` de al lado solo miraba `src`, así que un personaje con las mallas colgando de hijos seguía yendo a disco por cada hijo. Las dos cachés se llenan ahora en la misma pasada. | **Medido en Release** (`build-ninja-release`), 10 clones de `assets/animatedCharacter/Maw J Laygo.fbx`, 3 pasadas cada uno: **antes 25,3 / 24,4 / 24,5 ms por clon; después 1,8 / 1,7 / 1,8**. **13,6×**, y el spawn pasa de **1,5 frames a 60 fps** a un 11 % de frame. En Debug la mejora sale mucho mayor (159,3 → 2,8 ms/clon, 57×) justo por lo que avisa la cabecera: las deps de terceros van sin optimizar y el número no vale. Test: `test_clone_of_rigged_mesh_does_not_reread_disk` (`animator_tests.cpp:1029`), que no mide tiempo sino la **consecuencia observable** — se copia el FBX a un temporal, se carga, **se borra el fichero** y se clona: si el clon leyera disco perdería la malla. Visto en rojo primero (`FAIL: clone->isSkinned()`). Suite: 25/25 en Debug **y** en Release. | M |
| P4 | Media · **DESCARTADA POR MEDICIÓN** (2026-09-04) | `src/Core/Scene.cpp` | Proponía fusionar en una pasada los cuatro recorridos que corren tras propagar transforms. **No se hace: no es rentable.** | **Medido en Release**, escena en árbol (4 hijos por nodo, para que la propagación recurse de verdad), 200 iteraciones por dato, cada recorrido cronometrado por separado tras una pasada de calentamiento:<br><br>`1.000 nodos` — propagación **15,0 µs**, audio 3D **2,1**, luces **2,8**, listener **2,2**. Suma: **22,1 µs**.<br>`5.000 nodos` — **103,7 / 16,8 / 27,5 / 19,8**. Suma: **167,8 µs**.<br><br>Pero la suma **no es lo que la fusión ahorra**: `updateWorldTransforms` domina (103,7 de los 167,8) y hace trabajo real —una multiplicación de matriz por nodo— que una pasada fusionada sigue pagando entera. Lo que se ahorraría son los **tres recorridos extra**: **7,1 µs** con 1.000 nodos y **64,1 µs** con 5.000. Sobre un frame de 60 fps eso es **0,04 %** y **0,38 %**. Incluso a 144 fps y con 5.000 nodos se queda en el 0,92 %.<br><br>**El criterio era el 3 % y no se alcanza por un orden de magnitud.** Fila cerrada, igual que P14 del Renderer.<br><br>**Aviso sobre la medición**: la escena real del proyecto de pruebas **no vive en este repo**, así que esto es sintético. No cambia la conclusión —el coste es lineal en el número de nodos, y 5.000 ya es una escena grande—, pero si algún día aparece una escena con decenas de miles de nodos, el número que hay que rehacer es este. | M |
| P5 | Media · **DESCARTADA POR MEDICIÓN** (2026-09-04) | `include/DonTopo/Core/GameObject.h:97-104` | Proponía devolver `Collider*` crudo en vez de `shared_ptr`. **No se hace: no se nota.** | **Ablación de verdad, en Release**: se sustituyó `anyCollider()` por la búsqueda equivalente con puntero crudo dentro de `Scene::update`, se midió, y se revirtió.<br><br>Primero hubo que **arreglar la medición**: la escena sintética no tenía **ni un collider**, y ahí `anyCollider()` devuelve un `shared_ptr` nulo, que **no toca ningún atómico** — no estaba midiendo lo que la fila decía. Con 1.000 nodos y **un collider en cada uno**: `Scene::update` **97,2 µs** con `shared_ptr` y **97,9 µs** con puntero crudo. Repetido tres veces cada uno, los rangos se solapan enteros (97–111 µs los dos, con un pico de 190 por ruido del sistema).<br><br>**Sin diferencia medible.** El átomo, cuando existe, queda enterrado bajo el trabajo real de al lado. Y de paso queda el dato que faltaba: el paso de sincronización de colliders cuesta **72,6 µs por cada 1.000 nodos CON collider** (97,2 menos los 24,6 de la misma escena sin ninguno) — ahí sí hay coste, pero no está en el `shared_ptr`. | S |
| P6 | Media · ABIERTO | `src/Core/Scene.cpp:33-2633` → `src/Core/SceneSerialization.cpp` | Cierra H18. Mover el namespace anónimo entero (2.601 líneas) a su propio `.cpp`, exponiendo `nodeToJson`/`nodeFromJson` por un header interno. | **NO se propone por tiempo de compilación**: `docs/renderer-audit.md` H8 ya midió que partir un `.cpp` grande no lo mejora (15→6 ficheros, 27→**28** s), y aquí pasaría lo mismo. **Criterio binario, de acoplamiento**: tras el movimiento, `Scene.cpp` debe quedarse sin los `#include` de `PhysicsManager`, `AudioManager`, `ModelLoader`, `AsyncAssetLoader`, `FileManager`, `Cube/Sphere/Plane/Capsule` y `ScriptComponent` — 10 de sus 30. Si al terminar sigue necesitando alguno, el corte estaba mal elegido y se revierte. **Verificación del movimiento**: comparar a máquina el cuerpo nuevo contra las líneas originales (es un corta-pega puro; el cierre de cada función se localiza contando llaves, y si el script falla en todo, sospechar del script antes que del código). | L |
| P7 | Media · **HECHO** (2026-09-04) | `src/Core/Scene.cpp`, los 14 bloques de UI | Cierra H22. Las 28 lambdas fuera; sus **95 llamadas** (59 `readStr` + 36 `readBool`, contadas antes y después) pasan a las funciones del namespace, que ya reciben `warnings` y `contexto`. Hecho con un script y no a mano: 95 sitios en 14 regiones, y **dos bloques distintos usan la misma variable `p`** (progressBar y panel), así que una sustitución global habría cruzado los dos. El script valida cada bloque antes de tocarlo y aborta si algo no encaja. | **Criterio binario cumplido**: `grep -c "auto readStr = \[&\]"` y `grep -c 'readStr("'` dan **0**; el recuento de sustituciones (59 + 36) cuadra con el previo. Test `test_corrupt_ui_string_warns` (`camera_tests.cpp:537`) sobre **cuatro** componentes (button, panel, text, image), no uno: un test de un solo bloque dejaría 13 sin red. Cuatro tampoco son catorce — lo que cierra el hueco de verdad es que ya no queda ninguna lambda que copiar, y el test lo respalda. En el botón se corrompen a la vez un string y un float, que es el contraste del hallazgo. | M |
| P8 | Media · **HECHO** (2026-09-04) | `include/DonTopo/Core/Scene.h:60`, `src/Core/Scene.cpp:2662` | Cierra H3, y por la versión con callback: `Scene::setOnNodeRemoved` avisa **justo antes** del `erase`, con el subárbol todavía recorrible (que es lo que `Renderer::removeGameObject` necesita: recorre y libera las ranuras de todos los descendientes). El aviso va **tras** la guarda de `!node || !node->parent`, o borrar la raíz soltaría las ranuras de una escena viva. **Se quitaron los tres mecanismos**: `EditorContext::onDelete` + `EditorUI::setOnDelete` (un solo consumidor, `ScenePanel`), `ScriptManager::setOnDestroying` (disparaba en la línea de antes de `removeGameObject`, o sea el mismo evento) y la llamada a pelo de `DeleteGameObjectCommand`. Quedan **dos hooks menos y uno más**; el cableado pasa a los hosts (sandbox ×2 backends, runtime). **Efecto lateral bueno**: por el camino del panel, la GPU se liberaba ANTES de correr los `OnDestroy` de Lua —el objeto seguía vivo para el script pero ya sin recursos—; ahora se libera después. | **El criterio binario no se puede automatizar y se dice**: construir un `DeleteGameObjectCommand` en un test exige un `EditorRenderer`, que son 58 virtuales puras. Falsearlo cuesta más de lo que vale, así que esa punta se verifica en GUI. Lo que sí se prueba es el mecanismo, con 4 tests (`camera_tests.cpp:387-457`) y **3 sabotajes**: quitar el aviso tumba los dos tests de notificación; ponerlo antes de la guarda tumba el de "no avisar de lo que no se borra"; y moverlo DESPUÉS del `erase` no falla — **revienta con 0xC0000005**, que es la prueba de que el test comprueba el orden y no solo que llegue el aviso. 25/25 en Debug y Release. | M |
| P9 | Media · ABIERTO | `src/Core/Input.cpp:187, 190` | Cierra H5. Empujar a `s_actionDiagnostics` en los dos `continue` mudos, nombrando la acción y el binding descartado. El consumidor ya existe (`ScriptBindings.cpp:501`). | **Criterio binario**: un `input_actions.json` de fixture con un binding de dispositivo desconocido y otro con código de eje fuera de rango; tras `reloadActions()`, `takeActionDiagnostics()` devuelve 2 entradas y la segunda llamada devuelve 0 (se vacía al leer). Hoy la primera devuelve 0. | S |
| P10 | Media · ABIERTO | `src/Core/AnimatorComponent.cpp:52, 65` | Cierra H4. Partir el `reset()` como ya se hizo con `bindClips`/`rebindClips`: una variante que reindexa sin tocar los parámetros del usuario, para los caminos que pueden correr en Play. | **Criterio binario**: test que ponga un parámetro float a 0.75, llame a `removeState` de un estado que no es el actual y compruebe que el parámetro sigue valiendo 0.75 y `currentState()` no ha cambiado. Hoy los dos se pierden. | S |
| P11 | Baja · ABIERTO | `src/Core/Scene.cpp:2765-2770` | Cierra parte de H17. Cortar el recorrido en la primera coincidencia y, en builds de depuración, seguir recorriendo para hacer `assert` de que no hay una segunda. | **Criterio binario**: con P1 aplicado, el `assert` nunca debe saltar; sin P1, el repro de H1 lo dispara. Sirve de red por si aparece otra fuente de ids repetidos. | S |

---

## 4. Cobertura de tests

25 ficheros de test, 28.827 LOC en total (`cat engine/tests/*.cpp | wc -l`).

### 4.1 Lo que está protegido de verdad

| Área del Core | Dónde | Qué ejercita |
|---|---|---|
| `AnimatorComponent` | `animator_tests.cpp` (3.948 LOC) | Estados, transiciones, cross-fade, blend por parámetro, `bindClips`/`rebindClips`, root motion, y los dos caminos que pasaban `nullptr` (`:959-1004`) |
| `JobSystem` | `jobsystem_tests.cpp` (11 tests, `:22-293`) | Drenado en `shutdown`, cancelación antes de arrancar, **`shutdown` concurrente con espera del perdedor** (`:143`), excepción en un job, `reserveId` + `submitWithId` |
| `CameraComponent` | `camera_tests.cpp` (7.006 LOC) | Proyección, `viewFromWorld`, el orden `setFar` antes que `setNear` (`:517-521`, con un `near=3000` elegido a propósito para que el orden inverso lo delate) |
| `TransformDecompose` | `transform_decompose_tests.cpp` (4 tests) | Matriz singular, escala negativa, salidas opcionales |
| `Input` (ejes de mando) | `input_actions_tests.cpp:167-194` | Histéresis 0,5/0,4, gatillos renormalizados, dirección negativa no bindeable |
| `Scene`: ids del clon | `camera_tests.cpp:441-479` | Que el clon estrena id y que el original sigue resolviendo — **el camino de carga no** (H1) |
| `Scene`: invariante de cámara | `camera_tests.cpp:397-430` | El clon nunca se lleva la cámara, ni en la raíz ni en un descendiente |
| `Scene`: `reparent` | `scripting_tests.cpp:3458-3600` | Vía `Entity:SetParent` y vía `ReparentCommand`, incluido el reordenar dentro del mismo padre |
| `Scene::update` (física) | `physics_tests.cpp:1297-1311` | Las tres ramas del sync (dinámico, kinematic, static) |
| `Scene::collectCanvases` | `camera_tests.cpp` | Canvas anidados y cadena de anclaje |
| `DonTopo::Camera` (la del editor) | `camera_tests.cpp:7016` | **Nuevo 2026-09-04.** Front por defecto a -Z, clamp de pitch, `mouseSens`, `lookAlongAxis` (solo rota), `focusOn` (distancia, radio mínimo y el caso degenerado de estar YA en el centro) y que `getViewMatrix` es el `lookAt` que el resto del motor asume. `update()` fuera: pide un `GLFWwindow*` |
| `Scene::collectLights` | `camera_tests.cpp:7162` | **Nuevo 2026-09-04.** Recorte a `MAX_LIGHTS` devolviendo el TOTAL, vaciado de las salidas, posición y dirección desde el `worldTransform`, **la guarda de NaN con escala 0 en Z**, ángulos en coseno y el radio de Forward+ (area = ancho/2) |
| `Scene::fromJson` / `insertFromJson` | `camera_tests.cpp`, `scene_async_tests.cpp`, `audio_tests.cpp`, `animator_tests.cpp` | Round-trip, carga async, avisos, back-compat de `useGravity` |

### 4.2 Lo que no toca ningún test

| Sin cubrir | LOC | Por qué importa |
|---|---|---|
| ~~`src/Core/Camera.cpp`~~ | 98 | **CUBIERTO el 2026-09-04**: 7 tests. Queda fuera solo `update()`, que pide un `GLFWwindow*` (y con él la zona muerta del mando) |
| ~~`Scene::collectLights`~~ | 52 | **CUBIERTO el 2026-09-04**: 6 tests, uno por cosa que hace |
| `src/Core/Window.cpp` | 75 | Sin arnés (necesita GLFW). Aceptable, pero incluye el arranque oculto anti-flash |
| `src/Core/Engine.cpp` | 9 | No hay nada que probar (ver H21) |
| ~~El camino de carga de ids~~ | — | Era H1. **Cubierto desde 2026-09-04** por `test_load_advances_id_counter` (`camera_tests.cpp:509`) |
| `Scene::shutdown` | 11 (`:3146-3156`) | Qué limpia y qué deja (H6) |

### 4.3 El test que miente

`audio_tests.cpp:808-828`, `test_scene_updateAudioSpatial_walks_tree`. **Su
propio comentario lo admite** (`:806-807`: *"si alguien la borra de
`Scene::update`, el hallazgo H1 vuelve — este test no lo detectaría"*). Dos
motivos independientes:

1. Su **única** aserción es `CHECK(scene.findById(vacio->id) != nullptr)`
   (`:827`), que pasa igual si `updateAudioSpatial` es un no-op, si la llamada
   desaparece de `Scene::update:3118`, o si la posición que llega a la voz es
   la equivocada. No comprueba ni una posición.
2. La creación del clip está dentro de `if (clip3d)` (`:816`). Si
   `createAudioClipComponent` devuelve nulo —FMOD sin inicializar, o el
   directorio de trabajo no resuelve `assets/audio.mp3`— **el cuerpo entero se
   salta y el test pasa verde igual**. El fixture puede no llegar nunca a
   ejecutar la línea que dice proteger.

Es el patrón exacto que el encargo pide buscar. La pregunta que lo caza: *¿qué
tendría que romperse para que este `CHECK` fallara?* Aquí, nada de lo que el
test dice cubrir.

**Cómo se comprobó que los 13 tests nuevos (§4.1) no son de esa familia**: 15
sabotajes, **uno a uno**, cada uno tumbando el test que decía proteger y
revirtiendo antes del siguiente. En bloque no vale: tocar `updateVectors`
tumba TODOS los tests de `Camera` a la vez y la atribución se pierde — se
intentó primero así y hubo que rehacerlo. Los 15 cayeron por su test. Las
caídas colaterales que quedan son dependencias reales y no ruido: `focusOn`
llama a `lookAlongAxis`, y todo lo que lee `front` depende de
`updateVectors`.

Contraste con un test que **sí** sabe lo que protege:
`jobsystem_tests.cpp:207-210` escribe su propio sabotaje en el comentario
(*"quitar el `--m_inFlight`… y el assert final salta"*), y `camera_tests.cpp:487-494`
explica por qué existe (*"sin este test, mover el strip a `nodeFromJson`
—que parece la simplificación obvia— rompería el undo en silencio"*).

---

## 5. Prioridad

| Orden | Qué | Por qué primero / qué desbloquea |
|---|---|---|
| ~~1~~ | **P1** (H1, ids duplicados) — **HECHO 2026-09-04** | Era el único fallo de corrección Alta. Cerrado con test visto en rojo primero. **P11** sigue pendiente como red de seguridad (el `assert` de unicidad cazaría una segunda fuente de ids repetidos si aparece) |
| ~~2~~ | **P3** (H14, carga de disco síncrona al `Instantiate`) — **HECHO 2026-09-04** | Era la otra Alta. 24,5 → 1,8 ms por clon, medido en Release. Al hacerla salió un fallo **fuera del alcance de esta auditoría** que la tapaba: ver la nota de abajo sobre `ModelLoader::loadSkinned` |
| ~~3~~ | **P2 + P7** (H2 y H22) — **HECHO 2026-09-04** | Hechas juntas. P2 dejó H2 **reducido, no cerrado**: los 7 accesos crudos ya no existen, pero siguen los **57 `.value(...)`**. Y corrigió lo que la propia fila decía: solo 5 de los 7 perdían la escena; los otros dos perdían la malla en silencio, que es peor de diagnosticar |
| 4 | **P9 + P10** (H5 y H4) | Dos S independientes con criterio binario. P9 reconecta un canal de diagnóstico muerto desde el primer día; P10 impide que tocar el grafo en Play borre los parámetros de la partida |
| ~~5~~ | **Tests de `collectLights` y de `Camera.cpp`** (§4.2) — **HECHO 2026-09-04** | 13 tests, 15 sabotajes uno a uno. Ya hay red debajo de P4/P5 |
| ~~6~~ | **P4 + P5** (H12 y H13) — **MEDIDAS Y DESCARTADAS 2026-09-04** | Se midió antes de tocar, como decía la fila, y **el número dijo que no**: la fusión ahorraría el 0,38 % de un frame y el `shared_ptr` no se nota. Ninguna línea de producción cambió. La medición de P5 además **corrigió el razonamiento de H13**, que era falso para el caso común |
| ~~7~~ | **P8** (H3) — **HECHO 2026-09-04** | Se hizo la versión con callback, como decía la fila. Y resultó que no era "documentar una obligación" sino **borrar dos mecanismos duplicados**: el aviso estaba implementado tres veces |
| 8 | **P6** (H18, partir `Scene.cpp`) | La última a propósito. Es una L, no arregla ningún bug, y su beneficio es de acoplamiento, no de tiempo de build (que ya se midió y no mejora). Hacerla antes que P1/P3 sería mover 2.601 líneas por encima de dos fallos abiertos |
| — | **H19** (`GameObject.h`) | Sin propuesta a propósito. Está medido (33 includes, 28 miembros, 24 consumidores) para que la decisión se tome con datos, pero un mapa de componentes toca los 24 ficheros y no hay hoy un problema concreto que lo pague. Se reabre si aparece uno |

---

> **Estado al cerrar la pasada de lectura (2026-09-04)**: las 22 filas de
> hallazgo y las 11 de propuesta salieron **todas ABIERTAS**. Esa pasada fue de
> solo lectura: ni una línea de producción tocada, ni un build, ni una
> ejecución.
>
> **Trabajo hecho después, el mismo día**: **P1** (H1), **P3** (H14) —las dos
> Altas—, **P7** (H22) y **P2**, que deja **H2 reducido pero no cerrado**.
> Cuando se cierre otra fila, que se anote aquí y **en su propia fila**:
> corregir solo el resumen deja la corrección invisible para quien lea la
> tabla.
>
> Y la **cobertura** de `Scene::collectLights` y `Camera.cpp` (§4.1), que no
> era una fila pero iba antes que P4/P5 justamente para que esas dos dejen de
> ser apuestas.
>
> Y **P4 + P5** (H12, H13), medidas y **descartadas sin tocar una línea de
> producción**: la fusión de recorridos ahorraría el 0,38 % de un frame y el
> `shared_ptr` no se nota. Esas dos filas se conservan **por lo que
> descartan**, que es el único sitio donde vive esa medición.
>
> Y **P8** (H3), que resultó no ser "documentar una obligación" sino borrar
> dos mecanismos duplicados de aviso.
>
> **Lo que queda abierto: 15 filas** (H2 reducido incluido), todas Media o
> Baja, y ninguna con un caso de uso que empuje. Las que quedan son de
> contrato y de diagnóstico —H4, H5, H6, H7, H8, H9, H10, H11, H16, H17, H20,
> H21— más las dos de arquitectura (H18/P6, H19), que son L y no arreglan
> ningún fallo.

---

## Apéndice. Un fallo que esta auditoría NO podía encontrar

Salió al hacer P3, y su causa vive **fuera del alcance** (`Renderer`), así que no
es una fila de arriba. Se anota aquí porque lo destapó un test de Core y porque
invalidaba una promesa que sí está escrita en Core.

`ModelLoader::loadSkinned` (`src/Renderer/ModelLoader.cpp:180-187`) detectaba
correctamente el fallo de Assimp y acto seguido, **dentro del `if` cuya primera
condición es `!scene`**, desreferenciaba `scene` en un `printf` de depuración
(`scene->mNumMeshes`, …). Un FBX que no existe no lanzaba: mataba el proceso con
un **segfault mudo, exit 139, sin una línea en ningún log**.

Lo que importa está en Core: `nodeFromJson` envuelve la carga de malla en un
`try/catch` (`Scene.cpp:1683-1697`) cuyo comentario promete que un asset movido
o borrado deja el nodo sin mesh y *"el resto de la escena sigue cargando"*. Por
el camino skinned **no podía cumplirlo** — el editor se caía antes de llegar al
`catch`—, así que abrir una escena cuyo personaje ya no está en disco tumbaba el
proceso en vez de avisar. El hermano `load()` (`:87-89`) ya lanzaba bien; era
ese único sitio, y tenía toda la pinta de `printf` de depuración olvidado.

Cómo apareció: el test de P3 borra el FBX a propósito para comprobar que el clon
no depende del disco. En vez de fallar la aserción, el binario entero se caía
con 139. **Un `catch` no vale de nada si lo que hay debajo no lanza.**

Arreglado en el mismo commit, con su test
(`test_loadSkinned_missing_file_throws`, `animator_tests.cpp:93`).
