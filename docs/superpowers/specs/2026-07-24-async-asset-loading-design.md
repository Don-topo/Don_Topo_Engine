# Carga asíncrona de assets y uploads GPU sin stall

Fecha: 2026-07-24
Estado: diseño aprobado, pendiente de plan de implementación

## Problema

El motor no tiene threading. El único `std::atomic` del código es el contador de
IDs de `GameObject.cpp:8`. Todo — carga de disco, decodificación, upload a GPU,
física, scripts — corre en el hilo del bucle de `sandbox/src/main.cpp:256`.

Puntos de bloqueo medidos sobre el código:

| # | Sitio | Qué bloquea |
|---|---|---|
| 1 | `ModelLoader.cpp:85,175` | `Assimp::Importer::ReadFile()` síncrono en el frame del drop |
| 2 | `GpuResources.cpp:193,256` | `stbi_load` decodifica la imagen en el hilo principal |
| 3 | `AudioManager.cpp:76,102` | `FMOD::System::createSound` lee y decodifica el fichero entero |
| 4 | `GpuResources.cpp:62`, `GpuDevice.cpp:238` | `vkQueueWaitIdle` por cada one-time-command |
| 5 | `Renderer.cpp:2304,2383,2413,2433` | `vkDeviceWaitIdle` completo al añadir/quitar/rebuild de mesh |
| 6 | `Scene.cpp:721,1276` | `Scene::load` recorre N nodos cargando en serie |

El multiplicador es el 4. Un `addStaticMesh` de un mesh estático encadena unos
**11 `vkQueueWaitIdle`**: dos por los buffers de vértices e índices, y tres por
cada una de las tres texturas (transición → copia → transición). Cuarenta
objetos son unos **440 vaciados completos de la cola gráfica**. Mover la carga a
un worker sin tocar esto no gana casi nada, porque el upload sigue vaciando la
pipeline desde el hilo principal.

Hallazgo colateral: `EditorUI.cpp:170` llama a `reloadSceneFromJson` en cada
Stop de Play Mode, reconstruyendo toda la GPU. Ese hitch no viene del disco —
es puro `WaitIdle`, y lo arregla la capa B sin tocar la carga.

## Alcance

Esta spec cubre dos sub-proyectos que van juntos porque el primero sin el
segundo no rinde:

- **A** — carga asíncrona de meshes y texturas en hilos worker; audio vía
  `FMOD_NONBLOCKING`.
- **B** — uploads a GPU agrupados en un submit con fence, y eliminación de los
  cuatro `vkDeviceWaitIdle` del Renderer.

**Fuera de alcance, con spec propia pendiente:**

- **C** — frustum culling y batching. `Renderer.cpp:789-806` emite un draw call
  y un bind de descriptor set por objeto, sin culling, regrabando el command
  buffer entero cada frame. Es lo que limita el FPS base. **Ningún hilo arregla
  eso**, y esta spec no lo finge.
- **D** — solapar `PxScene::simulate()` con el resto del frame.
  `PhysicsManager.cpp:462-463` llama a `simulate` y `fetchResults(true)`
  pegados. La ganancia es pequeña mientras C siga sin hacer.

## Decisiones de producto

- **Drop de un asset suelto**: el `GameObject` no aparece hasta que su asset
  está listo. Sin placeholder, sin estados a medias.
- **Abrir una escena**: modal con barra de progreso. La edición queda vetada,
  pero la ventana sigue pintando frames y ofrece Cancel.
- **Play → Stop**: sigue siendo síncrono a propósito. Es determinista por
  diseño, y la capa B por sí sola ya lo acelera de forma notable.

## Arquitectura

```
main thread                     workers (N = clamp(hw_concurrency-1, 2, 8))
-----------                     ------------------------------------------
requestMesh(path) ──encola──►   Assimp::Importer::ReadFile()   (Importer local al job)
                                stbi_load() de las texturas del material
                                ──► LoadedMesh{ unique_ptr<Mesh>, vector<DecodedImage> }
pumpCompleted(2ms) ◄──buzón─────┘
  └─ addStaticMesh() desde datos ya en RAM
     └─ TransferBatch: 1 command buffer, 1 submit, 1 fence
```

**Regla dura: ni Vulkan ni FMOD se tocan desde un worker.** El worker produce
bytes en RAM; el hilo principal los sube. El audio no entra en el JobSystem —
`FMOD_NONBLOCKING` descarga en el hilo interno de FMOD y no escribimos código de
concurrencia para él.

| Fichero nuevo | Responsabilidad | Depende de |
|---|---|---|
| `Core/JobSystem.{h,cpp}` | Pool de hilos, cola FIFO, cancelación | nada del motor |
| `Renderer/AsyncAssetLoader.{h,cpp}` | Peticiones → jobs; buzón de resultados | JobSystem, ModelLoader, stb |
| `Renderer/TransferBatch.{h,cpp}` | Agrupa uploads en un submit + fence | GpuDevice |
| `Renderer/DeferredDelete.{h,cpp}` | Cola de destrucción diferida por frame | GpuDevice |
| `Editor/LoadingModal.{h,cpp}` | Overlay ImGui con progreso y Cancel | EditorUI |

## Capa A — trabajo CPU fuera del hilo principal

### JobSystem

```cpp
class JobSystem {
public:
    using JobId = uint64_t;
    void  start(unsigned threads = 0);   // 0 → clamp(hardware_concurrency()-1, 2, 8)
    void  shutdown();                    // drena la cola, hace join; idempotente
    JobId submit(std::function<void()> fn);
    void  cancel(JobId id);
    bool  idle() const;                  // cola vacía y ningún job en vuelo
private:
    std::deque<Job>          m_queue;
    std::mutex               m_mutex;
    std::condition_variable  m_cv;
    std::vector<std::thread> m_threads;
    std::atomic<bool>        m_stop{false};
    std::atomic<int>         m_inFlight{0};
};
```

Sin work-stealing, sin prioridades, sin futures. Una instancia por proceso,
propiedad de la aplicación y pasada por referencia — no singleton, para que los
tests monten la suya.

**Cancelación:** un job ya arrancado no se mata; no se puede interrumpir un
`ReadFile` a medias. `cancel` marca el `JobId`, el worker termina el trabajo y
el resultado se descarta al llegar al buzón. Cuesta CPU desperdiciada y evita
toda la complejidad de la interrupción cooperativa.

`cancel` tiene **un solo consumidor**: el botón Cancel del modal. No lo llama el
destructor de `GameObject`. Si lo hiciera, `Core` tendría que conocer
`Renderer/AsyncAssetLoader` — una dependencia invertida — y además sería
redundante: el pump ya descarta cualquier resultado cuyo `targetId` no exista en
la escena viva. Borrar un objeto con carga pendiente desperdicia el trabajo del
worker, nada más.

### AsyncAssetLoader

```cpp
struct DecodedImage {
    enum Slot { Albedo, Normal, ORM };
    Slot                 slot;
    int                  w, h;
    std::vector<uint8_t> pixels;    // RGBA, dueño; se libera solo
};

struct LoadedMesh {
    JobSystem::JobId          job;
    uint64_t                  targetId;   // GameObject::id, nunca un puntero
    std::string               path;
    std::shared_ptr<Mesh>     mesh;       // puede ser SkinnedMesh (loadAuto)
    std::vector<DecodedImage> images;
    std::string               error;      // no vacío = falló
};

class AsyncAssetLoader {
public:
    JobSystem::JobId requestMesh(const std::string& path, uint64_t targetId);
    void             cancel(JobSystem::JobId);
    std::vector<LoadedMesh> pumpCompleted(float budgetMs);   // hilo principal
    int              pending() const;
};
```

El worker decodifica **también las texturas del material**, no solo la
geometría. Sin eso, el `stbi_load` de un albedo 4K sigue en el hilo principal y
se pierde la mayor parte de la ganancia.

**Deduplicación por path.** Varias peticiones del mismo `path` con `targetId`
distintos comparten un único job, y por tanto un único `ReadFile`. Cada target
recibe su propio `LoadedMesh`, con el `Mesh` y los `DecodedImage` **copiados**
desde el resultado del job. La copia ocurre en el worker, no en el principal.

El tipo es `std::shared_ptr<Mesh>` porque es lo que devuelven
`ModelLoader::loadAuto` (`ModelLoader.h:48`) y lo que consume
`GameObject::setMesh` (`GameObject.h:39`). Podría compartirse el puntero, y se
copia igualmente **a propósito**: hoy cada nodo de `Scene::nodeFromJson` obtiene
su propio `make_shared<Mesh>` (`Scene.cpp:721`), y `Renderer::addStaticMesh` sube
cada `Mesh` a su propio par de buffers de GPU. Compartir no ahorraría memoria de
vídeo, cambiaría la semántica de propiedad actual, y dejaría a dos `GameObject`
apuntando al mismo `Mesh` mutable. Deduplicar el `ReadFile`, que es el coste
dominante, ya captura casi toda la ganancia.

**Corrección sobre el código actual:** el `std::unordered_map<std::string, bool>`
de `Scene.cpp:1109` cachea resultados de `hasBones()`, no mallas. Hoy dos nodos
con el mismo `sourcePath` hacen cada uno su `ModelLoader::load` completo. El
dedup de mallas de esta spec es **comportamiento nuevo**, no la conservación de
algo existente.

**Ciclo de vida y aliasing:**

- `LoadedMesh` viaja por valor con `std::move`. El worker no comparte nada
  mutable con el hilo principal: ni un puntero a `GameObject`, ni al `Renderer`.
- El buzón es un `std::vector<LoadedMesh>` bajo `m_mutex`. `pumpCompleted` hace
  swap-and-drain y procesa fuera del lock.
- El `Assimp::Importer` es local a cada job, en la pila del worker. Los
  `aiScene*` mueren con él; `ModelLoader` ya copia a `Mesh`, así que ningún
  puntero de Assimp cruza el límite de hilo.

**Errores:** una excepción dentro del job se captura en el worker y viaja en
`LoadedMesh::error`. Nunca cruza el límite de hilo — una excepción escapando de
un worker es `std::terminate`.

**Presupuesto:** 2 ms por frame. Si un solo mesh se pasa (no se puede partir la
construcción de un mesh de 2M vértices), se acepta el sobrecoste ese frame y el
pump se detiene hasta el siguiente.

## Capa B — GPU sin vaciar la cola

### TransferBatch

```cpp
class TransferBatch {
public:
    explicit TransferBatch(GpuDevice& gpu);
    VkCommandBuffer cmd();      // abre perezosamente; todas las ops comparten uno
    void            addStaging(VkBuffer, VkDeviceMemory);
    void            submit();   // vkEndCommandBuffer + 1 submit + 1 fence
    bool            complete() const;   // vkGetFenceStatus; NO bloquea
    void            reclaim();          // destruye staging y cmd buffer; exige complete()
private:
    VkFence m_fence = VK_NULL_HANDLE;
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> m_staging;
};
```

`GpuResources::createTextureImage`, `createNormalMapImage`,
`createSolidColorImage`, `createVertexBuffer` y `createIndexBuffer` pasan a
recibir un `TransferBatch&` en lugar de llamar a
`beginOneTimeCommands`/`endOneTimeCommands`.

Las transiciones de layout siguen siendo los mismos `vkCmdPipelineBarrier` de
hoy. **La corrección se mantiene porque las barreras ordenan dentro del command
buffer igual que ordenaban entre submits.** Lo que desaparece es el
`vkQueueWaitIdle` intercalado. Un pump de 40 objetos pasa de unos 440 vaciados
de cola a **1 submit y 1 fence**.

**Visibilidad diferida en lugar de semáforos.** El upload se envía en el frame N
y el objeto no se dibuja hasta que la fence señala, típicamente en el frame N+1.
El patrón de salto ya existe en `Renderer.cpp:790`
(`if (obj.vertexBuffer == VK_NULL_HANDLE) continue;`); se le añade la condición
de subida terminada. Esto evita plumbing de semáforos y encaja con la decisión
de producto de que el objeto no aparezca hasta estar listo.

**Lo que NO toca esta capa:** `GpuDevice::endOneTimeCommands` se mantiene tal
cual para el init de swapchain, shadow map y skybox. Son rutas de arranque donde
el `WaitIdle` no molesta y cambiarlas es riesgo sin premio.

### DeferredDeleteQueue

```cpp
class DeferredDeleteQueue {
public:
    void push(std::function<void(VkDevice)> destroyer);  // corre MAX_FRAMES+1 frames después
    void tick(VkDevice);        // 1 vez por frame, al inicio de drawFrame
    void flushAll(VkDevice);    // solo en shutdown, tras vkDeviceWaitIdle
};
```

`destroyRenderObject` y `destroySkinnedRenderObject` dejan de destruir en el
acto: encolan. Los cuatro call sites (`Renderer.cpp:2304`, `2383`, `2413`,
`2433`) eliminan su `vkDeviceWaitIdle`.

**Este es código destructivo y su modo de fallo es peor que el actual.** Hoy el
`WaitIdle` es lento pero imposible de equivocar. Con la cola, destruir un frame
demasiado pronto es un use-after-free en la GPU que no se reproduce de forma
fiable. Mitigaciones que forman parte del diseño, no de las buenas intenciones:

- El retraso es `MAX_FRAMES + 1`, no `MAX_FRAMES`. Un frame de margen sobre lo
  estrictamente necesario.
- `flushAll` **exige** un `vkDeviceWaitIdle` previo y solo se llama desde
  `Renderer::shutdown`.
- Desarrollo con capas de validación y sync validation activas. Una destrucción
  prematura la caza `VUID-vkDestroyImage-image-01000`.
- `destroyRenderObject` pasa a privada. El único camino público es el que
  encola, así que no se puede llamar al destructor inmediato por accidente desde
  un call site nuevo.

## Integración

**Regla de oro: nunca guardar un `GameObject*` a través del límite de hilo.** La
petición lleva el `id` de `GameObject.cpp:8`; al bombear se resuelve por `id`
recorriendo la escena viva. Si el usuario borró el objeto mientras cargaba, el
resultado se descarta. Un puntero cacheado aquí sería el mismo use-after-free
que ya se evitó con `liveCube` en `sandbox/src/main.cpp:293-296`.

### 1. Drop de FBX — `PropertiesPanel.cpp:121`

`loadMeshForSelected` pasa de cargar a encolar. El guard
`ctx.selected->hasMesh()` deja de bastar: durante la espera `hasMesh()` es falso,
así que un segundo drop encolaría una carga duplicada y el segundo resultado
pisaría al primero. Se añade `GameObject::pendingMeshJob` (`JobId`, 0 = ninguno)
y el guard pasa a `hasMesh() || pendingMeshJob != 0`. Al bombear se limpia.

`pendingMeshJob` es un `uint64_t` opaco en `Core`: `GameObject` no conoce al
`AsyncAssetLoader` ni lo cancela al destruirse. El pump descarta por `targetId`
inexistente, que cubre el caso sin invertir la dependencia.

El orden de registro se mantiene idéntico al de hoy
(`PropertiesPanel.cpp:144-150`): `addSkinnedMesh`/`addStaticMesh` **antes** de
`setMesh`, para que un fallo de GPU deje el `GameObject` intacto y el reintento
funcione en lugar de convertirse en un no-op silencioso.

### 2. Audio — `AudioManager.cpp:76,102`

`createSound` recibe `| FMOD_NONBLOCKING`. Retorna al instante con el
`FMOD::Sound*` en estado `FMOD_OPENSTATE_LOADING`. FMOD arranca con
`FMOD_INIT_NORMAL` (`AudioManager.cpp:29`), que es la API thread-safe, así que
no hace falta nada más por nuestra parte.

Único cambio de contrato: `playSound` sobre un sonido que aún carga debe
consultar `getOpenState()` y, si sigue cargando, no reproducir en lugar de
fallar.

### 3. Cargar escena — `Scene.cpp:1217` y `EditorUI.cpp:315`

`Scene::fromJson` gana un parámetro opcional `AsyncAssetLoader* loader = nullptr`:

- **`nullptr` → comportamiento de hoy, bit a bit.** Lo usan el restore de
  Play → Stop (`EditorUI.cpp:170`) y todas las suites de test existentes. No hay
  que reescribir nada en `engine/tests/`.
- **No nulo → asíncrono.** Los `GameObject` se crean completos (transform,
  jerarquía, colliders, scripts) pero sin mesh; cada `sourcePath` encola una
  petición. El cache de `hasBones()` de `Scene.cpp:1109` sigue igual y con su
  cometido de hoy; el dedup de mallas lo aporta `AsyncAssetLoader`, que es capa
  aparte.

Solo `EditorUI` (Load Scene) y el runtime pasan loader.

`staticRenderIndex` y `skinnedRenderIndex` no se serializan
(`Scene.cpp:1012,1121`), así que el orden no determinista de finalización de los
jobs no afecta al guardado.

### 4. Modal de progreso — `Editor/LoadingModal`

Ventana ImGui `AlwaysAutoResize | NoDecoration` centrada sobre el viewport, con
`completados / total` y barra. Mientras está activa, `EditorUI` veta el input de
edición (gizmos, drops, jerarquía) pero **sigue pintando frames**: la ventana no
se congela ni Windows la marca como "no responde".

Botón **Cancel**: cancela los jobs pendientes y deja la escena con lo cargado
hasta ese punto, que es un estado válido y guardable.

Se cierra cuando `AsyncAssetLoader::pending() == 0` **y** el último
`TransferBatch` ha señalado. Cerrar solo con `pending() == 0` mostraría un frame
con objetos todavía invisibles.

### 5. Bucle principal — `sandbox/src/main.cpp:256`

Dos llamadas nuevas, antes del `traverse` de la línea 294:

```cpp
renderer.tickDeferredDeletes();
editorUI.onAssetsLoaded(assetLoader.pumpCompleted(2.0f), scene, renderer);
```

### 6. Runtime exportado — `runtime/main.cpp:142`

`scene.load` recibe loader. El splash existente pasa de logo estático a logo con
progreso real, y cierra cuando la escena está completa en lugar de por
temporizador. Aquí sí se espera a que todo esté cargado antes del primer frame
de juego: un jugador no debe ver pop-in.

### Orden de apagado

`jobSystem.shutdown()` (join de todos los workers) → `vkDeviceWaitIdle` →
`deferredDeletes.flushAll()` → el `shutdown` actual. Si el JobSystem se destruye
después del Renderer, un worker en vuelo puede tocar memoria ya liberada.

## Verificación

Los tests son `main` y `assert` planos, sin framework, headless — el estilo de
`audio_tests.cpp:2`. Se mantiene.

**El riesgo específico de esta spec: un test de concurrencia mal escrito pasa
igual estando roto el código.** Un `assert(loader.pending() == 0)` después de un
`sleep` pasa aunque el buzón nunca se hubiera llenado. Cada test lleva su
comprobación de sabotaje: se rompe el código a propósito y se confirma que el
test **falla**. Sin esa evidencia, el test no cuenta como terminado.

### dt_jobsystem_tests

Puro, sin motor. Cada test corre **50 iteraciones**: un race que aparece una de
cada veinte veces no se caza en una sola pasada.

| Test | Sabotaje que debe hacerlo fallar |
|---|---|
| 1000 jobs incrementando un `atomic<int>` dan la suma exacta | quitar el `atomic` |
| `shutdown()` con cola llena ejecuta todos los jobs encolados | poner `m_stop` antes de drenar |
| `cancel()` de un job no arrancado impide su ejecución | ignorar el flag de cancelación |
| `shutdown()` dos veces no cuelga ni peta | quitar la idempotencia (deadlock en el join) |
| `start(1)` funciona con un solo hilo | cubre el clamp inferior |

### dt_asset_loader_tests

Con FBX reales de `assets/`.

- Cargar un FBX asíncrono da el mismo `Mesh` (contaje de vértices e índices,
  nombre de material, bounding box) que `ModelLoader::load` síncrono. La
  comparación es **contra la ruta síncrona, no contra constantes hardcodeadas**:
  si mañana cambia el importador, el test sigue siendo válido.
- Cuatro peticiones concurrentes al mismo path con `targetId` distintos producen
  un solo `ReadFile` y cuatro `LoadedMesh` de contenido idéntico pero con
  `mesh.get()` **distintos**. Comprobar los dos lados: que los datos coinciden y
  que las direcciones no. Solo lo primero pasaría también compartiendo el
  `shared_ptr`, que es justo lo que el diseño descarta.
- Un path inexistente devuelve `error` no vacío y `mesh == nullptr`, sin que
  ninguna excepción cruce el límite de hilo.
- `pumpCompleted(0.0f)` no procesa nada y no pierde resultados: el siguiente
  pump los entrega todos.
- Las texturas del material llegan decodificadas:
  `DecodedImage::pixels.size() == w*h*4` con `w > 1 && h > 1`. Se prueban los dos
  caminos de `GpuResources.cpp:189-195`: un FBX con textura embebida y otro con
  textura en disco.

### dt_scene_async_tests

- `fromJson(j, physics, audio, nullptr)` produce una escena cuyo `toJson` es
  idéntico al de hoy. Es el test que protege el restore de Play y las ocho
  suites existentes.
- `fromJson` con loader: los `GameObject` existen ya en el frame 0 con transform
  y jerarquía correctos, y `staticRenderIndex == -1` hasta que se bombea.
- Borrar un `GameObject` con carga pendiente y luego bombear no crashea, y el
  resultado se descarta. **Es el test con más valor de los tres** — cubre el
  use-after-free clásico de este patrón.

### Lo que no se puede testear headless

`TransferBatch` y `DeferredDeleteQueue` necesitan un device Vulkan. Van por otra
vía:

1. **Capas de validación y sync validation activas** durante todo el desarrollo.
   `VK_LAYER_KHRONOS_validation` con `validate_sync=true` caza la destrucción
   prematura y los hazards de escritura-lectura que ningún test funcional vería.
2. **Verificación manual guionizada**, en el formato de fixtures que ya existe:
   abrir una escena de 40 objetos; Play → Stop cinco veces; drop de un FBX
   durante Play; borrar un objeto mientras carga; cerrar la aplicación con
   cargas en vuelo. Cero errores de validación en consola.
3. **Medición antes y después** del frame time. El criterio es un número, no una
   sensación.

## Criterios de aceptación

| # | Criterio | Medida |
|---|---|---|
| 1 | Un drop de FBX no congela el editor | Ningún frame supera 33 ms durante la carga |
| 2 | Abrir una escena de 40 objetos | El modal responde; la ventana nunca se marca "no responde" |
| 3 | Play → Stop deja de dar hitch | El frame del Stop baja por debajo de 33 ms |
| 4 | Cero regresiones | Las ocho suites de test actuales pasan sin modificarlas |
| 5 | Cero errores de validación | En los cinco escenarios manuales |
| 6 | Cierre limpio con cargas en vuelo | Sin crash ni fuga reportada por las capas de validación |

## Deuda asumida conscientemente

El frustum culling y el batching (sub-proyecto C) siguen sin hacer, así que el
FPS base no sube con esta spec. Es una decisión, no un olvido.
