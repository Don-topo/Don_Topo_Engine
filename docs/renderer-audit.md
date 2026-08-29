# Auditoría del módulo Renderer

Fecha: 2026-08-28. Rama: `main` (`e906cd2`).

Alcance leído: `engine/src/Renderer/**`, `engine/include/DonTopo/Renderer/**` y los
ficheros de `engine/src/Editor/` que exponen ajustes de render (`EditorUI.cpp`,
`PropertiesPanel.cpp`, `PerformancePanel.cpp`, `ViewportPanel.cpp`,
`ProjectContext.cpp`).

Todo lo que se afirma aquí lleva `fichero:línea` verificado en el código. La RHI
(`EditorRenderer.h`, 79 virtuales de las que 66 son puras) **ya existe**: esta
auditoría no propone extraerla, sino cerrar sus huecos.

---

## 1. Inventario

### 1.1 Común a los dos backends

| Fichero / área | Responsabilidad | LOC | Backend |
|---|---|---|---|
| `include/DonTopo/Renderer/EditorRenderer.h` | RHI: lo que el editor y el runtime piden a un backend | 272 | común |
| `include/DonTopo/Renderer/RendererState.h` | 29 propiedades escalares de calidad y efectos | 235 | común |
| `include/DonTopo/Renderer/UniformBufferObject.h` | Layout std140 del UBO de escena, `Light`, `MAX_LIGHTS`, `SHADOW_CASCADES` | 58 | común |
| `include/DonTopo/Renderer/RenderBackend.h` + `src/Renderer/RenderBackend.cpp` | Elección y fallback del backend de arranque | 41 + 59 | común |
| `src/Renderer/InstanceBatching.cpp` | Agrupa draws por malla e instancia | 100 | común |
| `include/DonTopo/Renderer/Frustum.h` | `Culling::aabbVisible`, usado por los dos backends | 102 | común |
| `src/Renderer/ModelLoader.cpp` | Importa con Assimp a `Mesh` / `SkinnedMesh` | 515 | común |
| `src/Renderer/AsyncAssetLoader.cpp` | Carga y decodifica modelos en workers | 382 | común |
| `src/Renderer/SkinnedMeshAnimations.cpp` | Muestreo y evaluación de clips | 225 | común |
| `src/Renderer/SkinnedMeshPacking.cpp` | Concatena keyframes de todos los clips | 88 | común |
| `src/Renderer/SkinnedBounds.cpp` | AABB de personajes para el culling | 157 | común |
| `src/Renderer/{Cube,Sphere,Plane,Capsule}.cpp` | Geometría procedural de primitivas | 58/55/29/75 | común |
| `include/DonTopo/Renderer/{Mesh,Material,Vertex,MeshKey}.h` | Tipos de datos de malla y material | 23/19/13/18 | común |
| `include/DonTopo/Renderer/UiLayer.h` | Interfaz de la capa de UI que el backend dibuja encima | 103 | común |

### 1.2 Backend Vulkan

| Fichero / área | Responsabilidad | LOC | Backend |
|---|---|---|---|
| `src/Renderer/Renderer.cpp` | Orquesta frame, recursos, escena y presentación | 5253 | Vulkan |
| `include/DonTopo/Renderer/Renderer.h` | Declara el backend y todo su estado (~130 miembros) | 1075 | Vulkan |
| `src/Renderer/GpuDevice.cpp` | Instancia, device, colas y command pool | 243 | Vulkan |
| `src/Renderer/GpuResources.cpp` | Buffers, imágenes, texturas y samplers | 442 | Vulkan |
| `src/Renderer/TransferBatch.cpp` | Command buffer y fence de subidas agrupadas | 107 | Vulkan |
| `src/Renderer/DeferredDelete.cpp` | Retrasa destrucciones `kDelayFrames` frames | 35 | Vulkan |
| `src/Renderer/SharedGpuMesh.cpp` | Caché de mallas GPU con refcount y clave | 187 | Vulkan |
| `src/Renderer/Skybox.cpp` | Cubemap de fondo y su pipeline | 399 | Vulkan |
| `src/Renderer/SplashScreen.cpp` | Logo presentado antes de la carga pesada | 408 | Vulkan |
| `src/Renderer/Gizmos.cpp` | Líneas de depuración vía singleton global | 452 | Vulkan |
| `src/Renderer/Passes/ShadowPass.cpp` | Shadow map en 4 cascadas y su reparto | 513 | Vulkan |
| `src/Renderer/Passes/DepthPrepassPass.cpp` | Pre-pase de profundidad compartido por 5 efectos | 301 | Vulkan |
| `src/Renderer/Passes/SsaoPass.cpp` | Oclusión ambiental por compute y su blur | 425 | Vulkan |
| `src/Renderer/Passes/SsrPass.cpp` | Marcha de rayos y resolve de reflejos | 423 | Vulkan |
| `src/Renderer/Passes/FogPass.cpp` | Niebla volumétrica con in-scattering | 344 | Vulkan |
| `src/Renderer/Passes/MotionBlurPass.cpp` | Desenfoque de cámara por reproyección | 310 | Vulkan |
| `src/Renderer/Passes/BloomPass.cpp` | Cadena de mips y extracción HDR | 519 | Vulkan |
| `src/Renderer/Passes/AaPass.cpp` | FXAA, resolve de SSAA y acumulación TAA | 737 | Vulkan |
| `src/Renderer/Passes/ForwardPlusPass.cpp` | Culling de luces tiled y clustered | 511 | Vulkan |
| `src/Renderer/Passes/IblPass.cpp` | Cubemaps de irradiancia y entorno prefiltrado | 411 | Vulkan |
| `src/Renderer/Passes/ReflectionProbePass.cpp` | Bake y asignación de sondas de reflexión | 941 | Vulkan |
| `src/Renderer/Passes/SkinningPass.cpp` | Tres dispatches de deformación por huesos | 223 | Vulkan |

### 1.3 Backend DirectX 12

Todo el backend vive en un `.cpp` de 9932 líneas. El desglose es por rangos, no
por fichero, porque no hay más ficheros.

| Área (`src/Renderer/D3D12/D3D12Renderer.cpp`) | Responsabilidad | LOC | Backend |
|---|---|---|---|
| 59-467 | Structs de UBO/push constants y `static_assert` de layout | 409 | D3D12 |
| 180-360 | Constantes de calidad y reparto entero del SRV heap | 181 | D3D12 |
| 469-553 | `hresultToString`, `throwIfFailed`, `diagLog`, `narrow` | 85 | D3D12 |
| 554-1559 | Declaración de `struct Impl`: ~315 miembros, ~85 métodos | 1006 | D3D12 |
| 1560-1748 | Diagnóstico DRED, info queue, device removed | 189 | D3D12 |
| 1749-1894 | Fences, timestamps y avance de frame | 146 | D3D12 |
| 1895-2310 | Backbuffer, líneas de depuración, gizmos, subida de texturas | 416 | D3D12 |
| 2310-2741 | SRVs compartidos, depth de escena, root signature y PSOs PBR | 432 | D3D12 |
| 2742-3272 | UBO por frame y skinning por compute | 531 | D3D12 |
| 3273-3761 | Targets HDR/LDR/MSAA/TAA/SSR/motion blur y pipelines de bloom | 489 | D3D12 |
| 3762-4245 | IBL, buffers de Forward+ y recursos del cielo (rutas fijas) | 484 | D3D12 |
| 4246-4570 | Aplicar tamaño/muestras pendientes y culling Forward+ | 325 | D3D12 |
| 4571-5298 | TAA, SSR, motion blur y SSAO | 728 | D3D12 |
| 5299-5877 | Pre-pase, cielo, niebla, FXAA, resolve SSAA y pipelines de UI | 579 | D3D12 |
| 5878-6423 | Sondas: recursos, sincronización, asignación y horneado | 546 | D3D12 |
| 6424-7082 | Atlas y canvas de UI, niebla, bloom, composición, outline, AA | 659 | D3D12 |
| 7083-7626 | Sombras en cascada, instancias y view-proj del frame | 544 | D3D12 |
| 7627-8201 | `init()` completo, outline y pase principal de geometría | 575 | D3D12 |
| 8202-8632 | `drawFrame()` y resize diferido | 431 | D3D12 |
| 8634-9450 | API pública: cámara, luces, mallas, gizmos, viewport, UI | 817 | D3D12 |
| 9451-9633 | AA, sondas, métricas, selección y handles nativos | 183 | D3D12 |
| 9634-9932 | `shutdown()` e informe de fugas | 299 | D3D12 |
| `include/DonTopo/Renderer/D3D12/D3D12Renderer.h` | Fachada pImpl del backend | 350 | D3D12 |
| `src/Renderer/D3D12/D3D12Support.cpp` | Detección de adaptador capaz y `narrow` | 75 | D3D12 |

**Total:** 24 934 LOC en `src/Renderer/**` + 4 997 en `include/DonTopo/Renderer/**`.
Dos ficheros concentran el 61 %: `D3D12Renderer.cpp` (9932) y `Renderer.cpp` (5253).

---

## 2. Hallazgos

Severidad: **Alta** = corrompe estado, filtra recursos, tumba el proceso o miente
al usuario. **Media** = divergencia entre backends, fallo silencioso o deuda que
bloquea el crecimiento. **Baja** = corrección menor o documentación falsa.
Coste: S = sesión corta, M = varias horas, L = rediseño de fichero.

### 2.1 Transversales

| id | sev | fichero:línea | qué está mal | por qué importa | coste |
|---|---|---|---|---|---|
| H1 | Alta | `include/DonTopo/Renderer/UniformBufferObject.h:6` vs `include/DonTopo/Renderer/Passes/ForwardPlusPass.h:31` | Dos topes de luces incompatibles: `MAX_LIGHTS = 16` en el UBO y `kMaxLights = 256` en el culling. `Renderer.cpp:3115` recorta con `std::min` sin avisar; `ForwardPlusPass.cpp:360` reparte hasta 256. | Forward+ existe para escalar el número de luces y el shader PBR solo puede indexar 16: la feature está capada por el consumidor y el recorte es mudo. | M |
| H2 | Media | `src/Renderer/D3D12/D3D12Renderer.cpp:91` y `:2782` | El backend D3D12 escribe el literal `16` (`ShaderLight lights[16]`, `static_cast<size_t>(16)`) en vez de usar `MAX_LIGHTS`. | Subir el tope obliga a acordarse de un sitio que ningún grep de `MAX_LIGHTS` encuentra. | S |
| H3 | Media · **CERRADO en parte** | `src/Renderer/D3D12/D3D12Renderer.cpp:7262` y `:227-231` vs `src/Renderer/Passes/ShadowPass.cpp:364` | `computeCascades` y sus constantes (2048 px, λ=0.75, 500 de alcance) están reimplementadas a mano en D3D12. | Dos copias de la misma matemática: un ajuste en una deja las sombras distintas entre backends y nada lo detecta. | M |
| H4 | Media · **CERRADO** | `include/DonTopo/Renderer/Renderer.h:854` y `include/DonTopo/Renderer/D3D12/D3D12Renderer.h:269` | `ssaaFactor` es un escalar puro y vive duplicado en los dos backends en vez de en `RendererState.h`, que es exactamente lo que ese fichero documenta en su cabecera (`RendererState.h:17-19`). | Es la única propiedad de calidad con dos almacenes; el getter puede divergir del valor que persiste `ProjectContext.cpp:155`. | S |
| H5 | Media · **CERRADO** | `include/DonTopo/Renderer/Passes/ShadowPass.h:35` y `src/Renderer/Passes/ShadowPass.cpp:47` | `kShadowSize = 2048`, `kCascadeLambda`, `kShadowMaxDistance = 500` y `kCasterMargin` son constantes de compilación. | Es la única familia de calidad gráfica que no se puede ajustar ni por proyecto ni desde el menú de opciones de un juego exportado. | M |
| H6 | Media · **CERRADO** | `src/Renderer/Renderer.cpp:1873` vs `src/Renderer/D3D12/D3D12Renderer.cpp:9531-9535` | En Vulkan `statDrawCalls/statInstances/statCulled` solo cuentan con la captura activa; en D3D12 cuentan siempre porque `setPerfCaptureEnabled` es un no-op (`:9518`). | Las cifras del `PerformancePanel` no son comparables entre backends y el interruptor de captura miente en uno de los dos. | S |
| H60 | Alta · **CERRADO en parte** | `src/Renderer/Passes/ShadowPass.cpp:441` y `src/Renderer/D3D12/D3D12Renderer.cpp:9121` | Las sombras en cascada son una técnica de luz DIRECCIONAL y el motor las aplica a la luz 0 sea del tipo que sea. Además la dirección salía de `-normalize(posición)` —de la luz al ORIGEN DEL MUNDO—, ignorando el campo `direction` de la propia luz. | Con una luz de punto la sombra no diverge desde la luz, su tamaño no cambia con la distancia y apunta a donde no toca si la escena no está centrada en el origen. Y una luz direccional ignoraba su propia orientación: girar su gizmo no movía la sombra. **Cerrado el caso direccional y el de foco** (los dos usan su `direction`; un foco tiene cono, o sea direccion propia, y antes se le aplicaba la aproximacion de la de punto) y **cerrado en parte el de punto** por P22: ya no apunta al origen del mundo sino al centro de la escena. Le sigue faltando la divergencia, que es P21. La UI avisa de lo que le falta a cada tipo (`PropertiesPanel.cpp`, seccion Light). | S |
| H61 | Alta · **CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp:7925` (viewport) y `:7763` (el comentario que lo tapaba) | El shadow map de D3D12 se grababa espejado en vertical respecto a como lo lee el shader. La proyección estaba bien —no lleva la inversión de Y que sí lleva Vulkan— pero el pase grababa con viewport de altura POSITIVA, cuando todos los demás pases de ese backend usan altura negativa justamente porque los shaders asumen la orientación de Vulkan. | Con la luz cenital o la escena simétrica no se nota; con la luz en ángulo la sombra cae al otro lado del objeto. Afectaba a `pbr.frag` y a `fog.comp`, que derivan la UV igual. Lo tapaba un comentario que acertaba a medias: advertía de no volver a invertir la matriz —cierto— sin decir que la matriz y el viewport deciden JUNTOS. No lo detectó esta auditoría: salió al comparar los dos backends con la misma escena. | S |
| H62 | Alta · **CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp:7255` (`invViewProj` de la niebla) | `fog.comp` reconstruye la posición de mundo con `clip = vec4(uv*2-1, d, 1)`: da por hecho que la fila de ARRIBA cae en `ndc.y = -1`, que es la convención de Vulkan. Los pases gráficos de D3D12 compensan eso con el viewport de altura negativa, pero **un compute no tiene viewport**, así que no había dónde meterlo y la reconstrucción salía espejada. | La niebla se veía invertida en vertical. Lo tapaba un comentario que afirmaba «en D3D12 no hay ninguna [inversión] que meter». Mismo patrón que H61: contar la cadena entera, y si el shader es compartido la conversión va en la matriz cuando no hay viewport. | S |
| H63 | Alta · **CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp:7270` | El color de dispersión de la niebla era un tinte cálido A FUEGO —`vec3(1.0, 0.98, 0.94) * 0.7`— que ignoraba la luz de la escena, mientras Vulkan (`Passes/FogPass.cpp:279`) multiplica por el color y la intensidad reales. | Con la MISMA configuración los dos backends daban imágenes distintas: elegir blanco puro salía gris y subir la intensidad de la luz no aclaraba la niebla. Lo detectó el usuario comparando, y su primera hipótesis —«será cosa de cada renderer»— es justo la que habría dejado esto cerrado en falso. | S |
| H64 | Alta · **CERRADO** | `src/Renderer/Renderer.cpp:4942` (el comentario) y `Passes/DepthPrepassPass.cpp` | El pre-pase de profundidad de Vulkan excluía las mallas con huesos, justificándolo con que «el pass de sombras tampoco las mete». **Eso era falso**: `Renderer.cpp:3532` las dibuja con `bindSkinnedPipeline`. Y esa profundidad no es solo del SSAO: la NIEBLA marcha hasta ella. | Donde había un personaje, la niebla se calculaba hasta lo que hubiera detrás: un recorte con la forma del objeto de atrás. D3D12 ya lo hacía bien (`depthPrepassSkinnedPipeline`), o sea que cada backend tenía la mitad correcta. Efecto del arreglo: los personajes ahora también proyectan AO y entran en el SSR. | M |
| H65 | Alta · **CERRADO** | `src/Renderer/Passes/FogPass.cpp:268` | La niebla derivaba la dirección de la luz key por su cuenta con `-normalize(position)`, una COPIA del criterio de las cascadas. Al cambiar el de las cascadas (H60) y no éste, el in-scattering apuntaba a un lado y el shadow map estaba construido hacia otro. | Se extrajo a `keyLightDirection()` en `UniformBufferObject.h`, único sitio del criterio, y lo usan los tres consumidores. Tercer fallo del mismo tipo en la sesión: enumerar consumidores a mano y darlo por completo. | S |
| H66 | Alta · **CERRADO** | `src/Renderer/Renderer.cpp` (`rebuildShadowResources`) | Al recrear el shadow map se reescribía el binding 3 de los sets de mallas y de materiales, pero NO los del pase de niebla, que se lleva la vista y el sampler en su `Context`. | Dejaba un `VkImageView` destruido en un descriptor set de COMPUTE: error de validación en cada frame desde el arranque, con el PC ralentizado por el volumen de log. Regresión introducida por P6. | S |
| H67 | Alta · **CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp` (contorno skinned) | El contorno de un personaje se dibujaba DESPUÉS de que el pase de escena devolviera `outputVerts` a `UNORDERED_ACCESS`, así que el draw usaba un vertex buffer en estado inválido. | El contorno de selección de un personaje sencillamente no aparecía; el de un objeto estático sí, porque su vertex buffer no lo toca el skinning. Lo delató `[capa ERROR id=538]`. | S |
| H68 | Media · **CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp` (`drawOutline`) | El grosor del contorno era un `outlineWidth = 0.01f` plano —y su setter no lo llamaba nadie—, mientras Vulkan usa el 0,9 % del tamaño del objeto en mundo (`Renderer.cpp:1447`). | En objetos grandes el contorno era invisible. Se portó la fórmula, lo que exigió añadir `restMaxExtent` a los personajes de D3D12: solo los estáticos tenían caja. | S |
| H69 | Media · **CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp` (`recordFog`) | La ida transicionaba el depth de ESCENA y la vuelta el que devuelve `readableDepth()`, que con MSAA es OTRO recurso. | Con MSAA + niebla, el depth de escena se quedaba en lectura para siempre y el del pre-pase se tocaba desde un estado que no era el suyo. | S |
| H70 | Baja · **CERRADO (no era bug)** | `shaders/ssao.comp:36`, `shaders/ssr.comp:22`, `src/Renderer/Passes/SsaoPass.cpp:359`, `SsrPass.cpp:335` | Tras H62 se auditaron los otros tres compute que reconstruyen posición desde profundidad. **Motion blur es seguro**: reproyecta de clip a clip con `prevViewProj * inverse(currViewProj)`, misma fórmula en los dos backends, así que la inversión se cancela sola. **SSAO y SSR reciben `p11` con signos OPUESTOS** —Vulkan con Y-flip, D3D12 sin él— pese a que sus comentarios afirmaban que el flip hacía falta. | **Verificado visualmente en los dos backends: la imagen es idéntica.** Se cancela porque esos pases no salen de espacio de pantalla: reconstruyen con `p11`, sacan la normal de las propias posiciones reconstruidas y reproyectan con el mismo `p11`. No se cambia ningún signo —hacerlo «para que cuadre con el comentario» es el error que ya rompió cosas antes—; se corrigen los comentarios, que era lo único falso. | S |

### 2.2 Backend Vulkan — arquitectura

| id | sev | fichero:línea | qué está mal | por qué importa | coste |
|---|---|---|---|---|---|
| H7 | Alta | `src/Renderer/Renderer.cpp:1494-2242` | `recordCommandBuffer` son 750 líneas seguidas: queries de timestamp, culling, instancing, cuatro render passes, canvas de UI y blit headless. | Cada efecto nuevo engorda la misma función; no hay forma de reordenar ni probar un pase sin tocarla entera. | L |
| H8 | Alta | `include/DonTopo/Renderer/Renderer.h:27-43` y `:706-1075` | La clase declara ~130 miembros e incluye **por valor** los 12 headers de pases más 5 de UI. | Tocar cualquier header de pase recompila todo lo que vea `Renderer.h`; es lo que hace estructuralmente imposible adelgazar H7. | L |
| H9 | Media | `src/Renderer/Renderer.cpp:1728-1827`, `:3381-3420`, `:4858-4893` | El bucle «guardas de visibilidad + culling + `buildInstanceBatches` + bind/draw» está copiado tres veces (escena, sombras, depth pre-pass) con criterios que deben coincidir. | El propio código avisa de que si divergen el AO oscurece contra geometría que no se dibuja: es divergencia esperando a ocurrir. | M |
| H10 | Media | `src/Renderer/Renderer.cpp:2244-2515` vs `:3481-3651` | `createPipeline` y `createSkinnedGraphicsPipelines` duplican ~170 líneas de estado idéntico (rasterizer, depth, blend, dynamic, outline). | Un cambio de `frontFace` o de blend hay que hacerlo dos veces y solo se nota en la variante skinned. | M |
| H11 | Media | `src/Renderer/Passes/ShadowPass.cpp:16-37` y `src/Renderer/Passes/BloomPass.cpp:16-37` | `loadSpv` + `makeModule` están copiados literalmente en 14 ficheros (los 12 pases, `Skybox.cpp`, `SplashScreen.cpp`). | Validar `codeSize % 4`, cachear módulos o resolver rutas relativas al ejecutable exige replicarlo 14 veces. | S |
| H12 | Media | `src/Renderer/GpuResources.cpp:184-250`, `:252-306`, `:370-404`, `:406-440` | Cuatro funciones de subida casi idénticas que solo difieren en formato y origen de píxeles; la ruta sin `batch` hace tres `vkQueueWaitIdle` por textura (`GpuDevice.cpp:238`). | Duplicación cuádruple más un stall completo de cola por cada transición y copia en el camino síncrono. | M |
| H13 | Media | `src/Renderer/Passes/ReflectionProbePass.cpp:265-275` vs `src/Renderer/Renderer.cpp:2980-3017` | El pase de sondas reescribe los bindings 5 y 6 de los descriptor sets con su propia copia de la lógica del Renderer. | Añadir un binding IBL obliga a acordarse de dos sitios que no se referencian entre sí. | M |
| H14 | Media | `src/Renderer/Renderer.cpp:4155-4176` | El backend recorre el scene graph (`node->traverse`) y **escribe** `GameObject::staticRenderIndex` / `skinnedRenderIndex`. | El renderer conoce y muta el modelo de escena en vez de recibir listas: Core y Renderer no se pueden probar por separado. | M |
| H15 | Media | `src/Renderer/Renderer.cpp:1379-1474` y `:2036` | El contorno de selección (4 pipelines, 2 índices y su función de grabado) vive dentro del backend aunque solo lo use el editor. | Un runtime exportado paga los cuatro `vkCreateGraphicsPipelines` y su estado para algo que nunca dibuja. | M |
| H16 | Media | `include/DonTopo/Renderer/Gizmos.h:22` y `src/Renderer/Renderer.cpp:2037` | Los gizmos son un singleton estático global y mutable que cualquier parte del motor alimenta, y el pase de composición lo consume directo. | Estado global no thread-safe cuya limpieza depende de que `drawFrame` acuerde llamar a `clear()` en dos sitios (`:381` y `:499`). | M |

### 2.3 Backend Vulkan — corrección

| id | sev | fichero:línea | qué está mal | por qué importa | coste |
|---|---|---|---|---|---|
| H17 | Alta · **CERRADO** | `src/Renderer/Passes/SkinningPass.cpp:83` | El pool de descriptores del skinning está clavado a `maxSets = 16`, sin recreación ni fallback. | El personaje nº 17 hace lanzar `std::runtime_error` en `Renderer.cpp:3812` y tumba la carga de escena. | S |
| H18 | Alta · **CERRADO** | `src/Renderer/Renderer.cpp:2907` | `m_descriptorPool` se dimensiona una sola vez con `(m_objects.size() + 128) * MAX_FRAMES` y nunca se recrea al crecer la escena. | Pasado el margen de 128, `vkAllocateDescriptorSets` falla y `Renderer.cpp:2949` lanza en mitad de un Load Scene asíncrono. | M |
| H19 | Alta | `src/Renderer/Renderer.cpp:4131-4137` y `:4139-4147` | `removeStaticObject` / `removeSkinnedObject` sueltan los recursos pero la entrada **nunca sale** de `m_objects` / `m_skinnedObjects`. | Cada ciclo cargar/descargar escena o Play/Stop alarga todos los bucles por frame y agranda el SSBO de instancias sin límite. | M |
| H20 | Alta · **CERRADO** | `src/Renderer/Skybox.cpp:39-46` | `Skybox::init` no destruye nada de lo anterior: una segunda llamada pisa imagen, memoria, vista, sampler, pool, layout y pipeline. | **No es alcanzable hoy**: `initSkybox` se llama una sola vez, desde `sandbox/src/main.cpp:826` y `runtime/main.cpp:409`. Se arregla porque es prerrequisito de P5 (el selector de skybox), que lo llamaría por segunda vez. Cambiar el skybox filtraría un cubemap entero por cambio. `IblPass.cpp:262` sí contempla la re-entrada; `Skybox` no. | S |
| H21 | Media · **CERRADO** | `src/Renderer/Renderer.cpp:181-195` | El bbox de auto-fit sale de `bMin = +max`, `bMax = -max` y `maxDim` se calcula sin comprobar que `meshes` traiga algo: con lista vacía sale `-inf`. | `Renderer.cpp:821-822` deriva near/far de `m_cameraDistance`, así que la proyección del editor queda con infinitos. `refitCameraRange()` sí se guarda; este camino no. | S |
| H22 | Media · **CERRADO** | `src/Renderer/Passes/BloomPass.cpp:179-188` + `src/Renderer/Renderer.cpp:4664` y `:2028` | Con `renderExtent` por debajo de 4 px `m_mipCount` queda a 0, `createBloomImages` sale antes de `createCompositeSets`, y la composición bindea igual un `m_compositeSets[frame]` en `VK_NULL_HANDLE`. | Colapsar el panel Viewport a unos pocos píxeles bindea un descriptor set nulo: error de validación o device lost. | S |
| H23 | Media | `src/Renderer/Renderer.cpp:1512` y `:3442` | `ensureInstanceCapacity` dimensiona el SSBO solo con `m_objects.size()`; los objetos skinned del pase de sombras se saltan con un `break` mudo cuando no caben. | Una escena con muchos personajes pierde sombras sin error, sin aviso y sin contador que lo delate. | S |
| H24 | Media · **CERRADO** | `src/Renderer/Renderer.cpp:2746-2759` | `createUniformBuffers` ignora los `VkResult` de `vkCreateBuffer`, `vkAllocateMemory`, `vkBindBufferMemory` y `vkMapMemory`. | Un fallo de memoria deja `m_uniformBuffersMapped` a basura y el `memcpy` de `updateUniformBuffer` (`:3133`) escribe en un puntero inválido. | S |
| H25 | Media | `src/Renderer/Renderer.cpp:4278` | `replaceStaticTextureWithMissing` hace `vkUpdateDescriptorSets` sobre un set que un command buffer en vuelo puede tener bindeado, sin `UPDATE_AFTER_BIND`. | Los **recursos** sí están a salvo (`:4207` los encola en el borrado diferido), pero actualizar el set en sí sigue siendo uso inválido de Vulkan. | S |
| H26 | Media | `include/DonTopo/Renderer/Renderer.h:245-246` vs `src/Renderer/Renderer.cpp:4207` | El header promete «sincroniza con `vkDeviceWaitIdle` antes de tocar el descriptor set»; el `.cpp` dice literalmente «Sin `vkDeviceWaitIdle`». | El header es lo primero que lee quien va a tocar esa función, y dice lo contrario que el código. | S |
| H27 | Media | `src/Renderer/GpuDevice.cpp:57-67` | Se activa `VK_LAYER_KHRONOS_validation` sin comprobar antes con `vkEnumerateInstanceLayerProperties` que exista. | En una máquina sin el SDK la build Debug muere en `vkCreateInstance` con «failed to create Vulkan instance!» y ni una pista de la causa. | S |
| H28 | Media | `src/Renderer/Renderer.cpp:938` | El present mode está clavado a `VK_PRESENT_MODE_FIFO_KHR` sin consultar los soportados. | Impide medir por encima de la tasa de refresco y no deja ofrecer MAILBOX/IMMEDIATE en el juego exportado. | S |
| H29 | Baja · **CERRADO** | `src/Renderer/Renderer.cpp:884` | `surfaceFormats[0]` se lee sin comprobar que `formatCount > 0`. | Acceso fuera de rango sobre un vector vacío durante el arranque, sin diagnóstico. | S |
| H30 | Baja | `src/Renderer/Skybox.cpp:91-99` | Si una cara falla o no coincide de tamaño se lanza con los `stbi_uc*` anteriores todavía reservados. | Fuga de CPU en el error más frecuente del usuario (caras de distinto tamaño), además de una excepción que sube al editor. | S |

### 2.4 Backend DirectX 12

| id | sev | fichero:línea | qué está mal | por qué importa | coste |
|---|---|---|---|---|---|
| H31 | Alta · **CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp:7052` (donde faltaba la consulta) | `recordBloomAndComposite` no consultaba nunca el getter `bloomEnabled()`: el valor se guardaba (`setBloomEnabled` → `setBloomEnabledFlag`, `:9336`) y nadie lo leía. *(Rectificación: la primera versión de esta fila decía «cero ocurrencias de `bloomEnabled` en todo el backend». Era un artefacto de un grep sensible a mayúsculas —`setBloomEnabled` sí existía—; lo cierto es que el **getter** no se consultaba.)* | Apagar el bloom en el editor no cambiaba el render bajo D3D12 y se pagaban 9 dispatches por frame; en Vulkan sí se apaga (`Renderer.cpp:1980`). No basta con mandar `intensity = 0`: el shader hace `color += bloom * intensity` siempre y la cadena es `R16G16B16A16_FLOAT` sin inicializar, con lo que `inf * 0` daría NaN — el mismo peligro que `Renderer::setBloomEnabled` documenta en Vulkan. | S |
| H32 | Alta | `src/Renderer/D3D12/D3D12Renderer.cpp:9112-9123` | `removeGameObject` solo hace `setObjectMeshVisible(false)` y pone el índice a `-1`: no libera buffers, texturas ni el hueco de descriptores, y `tickDeferredDeletes` (`:8872`) es un no-op declarado. | Vulkan sí libera (`Renderer.cpp:4157`). Crear y borrar objetos en el editor filtra VRAM toda la sesión y agota los 512 huecos de `kMaxObjectSlots` sin que nada lo diga. | M |
| H33 | Alta · **CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp:1755-1756` | Si `queue->Signal` falla, `waitForGpu` hace `return` sin esperar a nadie. | Todos los llamantes (resize, cambio de AA, `clearStaticMeshes`, `shutdown`) liberan recursos justo después: uso tras liberar en GPU y corrupción silenciosa. | S |
| H34 | Alta · **CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp:1871` | Igual en `moveToNextFrame`: sale sin avanzar `frameIndex` ni recolocar `fenceValues`. | El frame siguiente resetea un `ID3D12CommandAllocator` cuyas listas pueden seguir ejecutándose. | S |
| H35 | ~~Alta~~ → **Baja · CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp:4601-4602` y `:4621` | La guarda `if (!clustered && !depthReady) return;` dejaba pasar al modo Clustered hacia tres usos incondicionales de `prepassDepthAllocation`: la barrera de `:4621`, su inversa en `toScene[2]` (`:4676`) y el bind de `kSrvPrepassDepth` en la tabla 6 (`:4653`). La guarda afirmaba lo contrario de lo que hace el cuerpo. | **Corregida la severidad: NO es alcanzable.** `init()` llama a `createSsaoTargets()` sin condición (`:7991`) y `releaseSsaoTargets()` solo corre desde dentro de `createSsaoTargets` —recreando acto seguido (`:5318`)— y desde `shutdown()` (`:9907`), así que el puntero nunca es nulo mientras se dibujan frames. Queda como endurecimiento: la contradicción se cobra la pieza el día que los targets del SSAO se creen solo con el efecto encendido. | S |
| H36 | Alta · **CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp:6350` (creación) y `:9950` (donde faltaba) | `probeDepthAllocation` se crea con D3D12MA y no la liberaba nadie: no va en `releaseProbe` (`:6532`) —y hace bien, porque es UNA sola compartida por todas las sondas— pero tampoco estaba en `shutdown()`. | D3D12MA hace `assert()` al destruirse con bloques vivos: en Debug es abort al cerrar con exit 3 y ventana de Windows, sin volcado. Mismo patrón ya arreglado para los atlas de UI (`:9970`). Barrido completo de los 38 `Allocation*` de `Impl` contra el camino de `shutdown()`: era el único huérfano. | S |
| H37 | Alta | `src/Renderer/D3D12/D3D12Renderer.cpp:554-1559` | God object: `struct Impl` con ~315 miembros y ~85 métodos dentro de un `.cpp` de 9932 líneas, sin una sola clase `*Pass`. | Vulkan tiene el mismo trabajo repartido en 12 clases con ciclo de vida propio; aquí cualquier cambio recompila todo y no hay frontera testeable. | L |
| H38 | Media · **CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp:9537-9538` | `forwardPlusAvgPerCell()` y `forwardPlusOverflowCells()` devuelven `0.0f` y `0` cableados, aunque `fpStatsAllocation` (`:4407`) se enlaza como UAV y el compute escribe en él (`:4539`). Solo falta el readback. | El panel enseña «0 desbordes» cuando puede haber celdas saturadas: el diagnóstico de Forward+ no está ausente, es **falso** (lo pinta `EditorUI.cpp:1427-1431`). | S |
| H39 | Media · **CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp:8862-8869` (el `(void)radii`) y `:4605` (el radio que se usaba) | `setLightRadii` descartaba la lista con un `(void)` y el reparto por celdas tiraba siempre de `params.x`, así que `forwardPlusLightRadius()` no se leía en ningún punto del backend. | El slider «Light radius» (`EditorUI.cpp:1422`) no hacía nada bajo D3D12. Ahora sigue el mismo criterio que `ForwardPlusPass.cpp:387`: el radio que mande el llamante para esa luz, y si no lo hay, el global. | S |
| H40 | Media · **CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp:4072-4076` | Las seis caras del cielo están escritas a fuego como `assets/skybox/*.png` e `initSkybox()` no se sobrescribe: se queda el default vacío de `EditorRenderer.h:58`. | El skybox del proyecto se ignora bajo D3D12, y con él el IBL global que se convoluciona de ese cubemap. `GameExporter.cpp:503` ya documenta el hardcode como limitación del runtime. | M |
| H41 | Media · **CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp:9518` | `setPerfCaptureEnabled(bool) {}` es un no-op: los timestamps se graban siempre (`:8266-8267`). | El gate del `PerformancePanel.cpp:145` no apaga nada bajo D3D12; ver también H6. | S |
| H42 | Media | `src/Renderer/D3D12/D3D12Renderer.cpp:1912-1988` | Toda subida (`uploadBuffer` / `uploadTexture`) resetea el allocator, ejecuta y hace un `waitForGpu()` completo; `hasPendingUploads()` no se sobrescribe. | Cargar una escena hace un stall de GPU por recurso, mientras Vulkan tiene camino asíncrono (`Renderer.cpp:4065`). | L |
| H43 | Media | `src/Renderer/D3D12/D3D12Renderer.cpp:271-360` | Reparto del SRV heap totalmente estático por índices calculados (`kMaxObjectSlots = 512`, `kMaxSkinnedSlots = 16`, `kMaxUiAtlases = 16`, `kMaxProbes = 8`, `kImGuiReserved = 16`) sin free-list; el hueco sale de `objects.size()` (`:8767`). | Los huecos no se reciclan nunca (ver H32) y, pasado el tope, los objetos se dibujan con las texturas neutras sin que nadie lo diga. | M |
| H44 | Media | `src/Renderer/D3D12/D3D12Renderer.cpp:7805` | El heap de RTV se dimensiona `kFrameCount + 6 + 6` y sus índices se escriben a mano en 8 sitios (`:3407`, `:3435`, `:3483`, `:3514`, `:3565`, `:4684`, `:6273`, `:8367`) sin un solo `static_assert`. | Encaja exacto por casualidad; añadir un target escribe fuera del heap y D3D12 no lo valida al crear la vista. | S |
| H45 | Media · **CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp:8510` y `:8256-8259` | `Close()`, `allocator->Reset()` y `commandList->Reset()` fallidos hacen `return` mudo, y en el segundo caso después de que `syncProbes` ya haya enviado trabajo. | Frames perdidos en silencio con estado a medias (timestamps sin resolver, sondas marcadas); el resto del fichero sí registra con `diagLog`. | S |
| H46 | Media | `src/Renderer/D3D12/D3D12Renderer.cpp:9634-9932` | `shutdown()` no suelta `timestampHeap` ni `timestampReadback` (que además queda mapeado) antes de `device.Reset()` y del `ReportLiveObjects` de `:9922`. | El informe de fugas de depuración sale siempre sucio y deja de servir para detectar fugas reales. | S |
| H47 | Baja | `include/DonTopo/Renderer/D3D12/D3D12Renderer.h:28-32`, `:216-218`, `:266-278` | Los comentarios afirman que no hay mallas ni post ni UI, que el rango de cámara es fijo, que SSAA no está y que sondas y métricas «todavía no». Las cuatro cosas están implementadas (`:4260`, `:5958`, `:9522`, `:9250`). | El header es lo primero que se lee para decidir si un efecto existe; hoy contradice al `.cpp` en cuatro puntos. | S |
| H48 | Baja | `src/Renderer/D3D12/D3D12Renderer.cpp:540` vs `src/Renderer/D3D12/D3D12Support.cpp:19` | `narrow()` está duplicada; la copia del renderer es la que traduce el JSON de D3D12MA en el camino de diagnóstico. | Dos implementaciones de conversión que divergen sin aviso. | S |
| H59 | Media · **CERRADO** | `src/Renderer/D3D12/D3D12Renderer.cpp:7072` (`ldrAllocation`) y `:8484-8494` (`viewportAllocation`) | Ningún render target reservado por D3D12MA recibía el `Discard`/`Clear`/`Copy` que exige el primer uso de un heap `D3D12_HEAP_FLAG_CREATE_NOT_ZEROED`; `DiscardResource` no aparecía ni una vez en las 9932 líneas y `ClearRenderTargetView` solo cubría el RT de escena (`:8077`) y el backbuffer (`:8612`). | Uso inválido de la API, y sobre todo dos `[capa ERROR id=1422]` por arranque y por CADA recreación por resize, que es lo que enterraba los errores de verdad en `d3d12_diag.log` (ver H46). No lo detectó esta auditoría: salió al ejecutar el editor. | S |

### 2.5 Superficie del editor

| id | sev | fichero:línea | qué está mal | por qué importa | coste |
|---|---|---|---|---|---|
| H49 | Media | `src/Editor/EditorUI.cpp:1059-1432` | Ninguno de los 39 ajustes de render pasa por `UndoManager` / `PropertyCommand`, a diferencia de lo equivalente en `PropertiesPanel.cpp:651` y `:704`. | Un arrastre accidental sobre bloom o SSAO no se deshace con Ctrl+Z aunque el resto del editor sí lo permita. | M |
| H50 | Media · **CERRADO** | `src/Editor/EditorUI.cpp:1581` | `setWireframeMode` no llama a `saveProjectSettings()`, y `wireframeMode` no existe ni en `ViewSettings` (`ProjectContext.h:55-95`) ni en la escritura/lectura de `ProjectContext.cpp:117-159` / `:236-289`. | Es el **único** campo de `RendererState` que no persiste: se pierde al reabrir el proyecto y el juego exportado nunca lo ve. | S |
| H51 | Media | `src/Editor/EditorUI.cpp:1089` y `src/Editor/PropertiesPanel.cpp:755` | Llaman al `constexpr` estático `Renderer::probeMemoryBytes()` (`Renderer.h:301`, clase concreta de Vulkan) desde código que por lo demás solo habla con `EditorRenderer`. | Con el backend DX12 activo la cifra de MB por sonda es la del backend que no está corriendo. | S |
| H52 | Media · **CERRADO** | `src/Editor/EditorUI.cpp:1427-1431` | Pinta `forwardPlusAvgPerCell()` y `forwardPlusOverflowCells()` sin saber que bajo D3D12 son constantes (H38). | «Luces/celda: 0.0» es falso y el aviso naranja de celdas desbordadas no salta jamás con DirectX 12. | M |
| H53 | Baja · **CERRADO** | `src/Editor/EditorUI.cpp:1364-1377` | El bucle `for (int s = 2; s <= 8; s *= 2)` no ofrece 1x y, si `maxMsaaSamples() == 1`, no dibuja ningún RadioButton: el `SameLine()` de `:1376` cuelga el «(max 1x)» del Combo de `:1311`. | En una GPU sin MSAA el modo se puede seleccionar igual y no queda ni un control ni un aviso de que no se aplica. | S |
| H54 | Baja · **CERRADO** | `src/Editor/EditorUI.cpp:1350` | El slider capa `ssaaFactor` a `[1.25, 2.0]` mientras `setSsaaFactor` no clampea nada y el default del core es 2.0 (`Renderer.h:854`). | Contradice la regla de no capar en UI lo que el core acepta: 1.0 y valores por encima de 2 solo se alcanzan editando el `project.json` a mano. | S |
| H55 | Baja · **CERRADO** | `src/Editor/EditorUI.cpp:1347-1348` | `static float pendingFactor` cuyo refresco está guardado por `ImGui::IsAnyItemActive()`, que es global y no «este slider concreto». | Cualquier otro widget activo congela el valor mostrado, y el `static` sobrevive al cambio de proyecto. | S |
| H56 | Baja | `src/Editor/EditorUI.cpp:1084-1090` | El `BeginDisabled(probes == 0)` solo cubre el MenuItem: «Sondas: 0» y «Último bake: 0.00 ms» se pintan igual sin ninguna sonda. | Un 0.00 ms se lee como bake instantáneo en vez de «nunca bakeado», que es lo que `PropertiesPanel.cpp:753` sí distingue con «sin bakear». | S |
| H57 | Baja | `src/Editor/EditorUI.cpp:1124`/`:1166`/`:1211`/`:1265`/`:1398`/`:1399`/`:1426` vs `src/Editor/PerformancePanel.cpp:197-205` | Los siete tiempos de GPU por pase se pintan en dos superficies con formato distinto: texto suelto `%.3f ms` frente a tabla con porcentaje del total. | Dos sitios que mantener sincronizados para el mismo dato, y el del menú View no da el porcentaje que hace útil la comparación. | M |
| H58 | Baja | `src/Editor/EditorUI.cpp:993` y `:1054-1433` | Los 39 controles de render viven dentro de un `BeginMenu("View")`, no en un panel acoplable como Performance o Properties. | Afinar bloom, SSAO o niebla exige ver el viewport mientras se arrastra, y un popup de menú lo tapa y se cierra. | L |

---

## 3. Paridad de backends

Cubre **todas** las propiedades de `RendererState.h` más el resto de features del
módulo. «Parcial» = implementado con una limitación demostrable. «Ausente» =
sin override, o con override que devuelve constante / no hace nada.

### 3.1 Propiedades de `RendererState.h`

| Feature | Vulkan | D3D12 | Nota |
|---|---|---|---|
| `ambientEnabled` / `ambientIntensity` | Implementado — `Renderer.cpp:3116` | Implementado — `D3D12Renderer.cpp:2811` (sonda en `:2786`) | Mismo hueco del UBO, con `static_assert` en `D3D12Renderer.cpp:106` |
| `bloomEnabled` | Implementado — `Renderer.cpp:1980` | **Implementado (era ausente)** — `D3D12Renderer.cpp:7052` | El getter no se consultaba: el flag se guardaba y la cadena corría igual. Cerrado, ver H31 |
| `bloomThreshold` / `bloomKnee` / `bloomIntensity` | Implementado — `Passes/BloomPass.cpp:179` | Implementado — `D3D12Renderer.cpp:6862` (pipelines en `:3620-3761`) | Push constants en los dos |
| `ssaoEnabled` | Implementado — `Passes/SsaoPass.cpp:345` | Implementado — `D3D12Renderer.cpp:5299` | |
| `ssaoRadius` / `Bias` / `Intensity` / `Power` | Implementado — `Passes/SsaoPass.cpp:345` | Implementado — `D3D12Renderer.cpp:5299` | Mismos `ssao.comp` y `ssao_blur.comp` |
| `ssrEnabled` | Implementado — `Passes/SsrPass.cpp:288` | Implementado — `D3D12Renderer.cpp:4820` | Trazado + resolve, compute en ambos |
| `ssrMaxDistance` / `Thickness` / `MaxSteps` / `EdgeFade` / `Intensity` | Implementado — `Passes/SsrPass.cpp:288` | Implementado — `D3D12Renderer.cpp:4738-4900` | `SsrPush` compartido |
| `fogEnabled` | Implementado — `Passes/FogPass.cpp:213` | Implementado — `D3D12Renderer.cpp:6801` | |
| `fogDensity` / `HeightFalloff` / `BaseHeight` / `Scatter` / `Anisotropy` / `Steps` | Implementado — `Passes/FogPass.cpp:213` | Implementado — `D3D12Renderer.cpp:6840` | Mismo `fog.comp` |
| `motionBlurEnabled` | Implementado — `Passes/MotionBlurPass.cpp:222` | Implementado — `D3D12Renderer.cpp:4991`, llamada en `:8470` | |
| `motionBlurIntensity` / `MaxRadius` / `Samples` | Implementado — `Passes/MotionBlurPass.cpp:222` | Implementado — `D3D12Renderer.cpp:4901-5058` | En D3D12 el pase no lleva `markTimestamp`: su coste no aparece en ninguna métrica |
| `aaMode = Fxaa` + `fxaaSubpix` / `EdgeThreshold` / `EdgeThresholdMin` | Implementado — `Passes/AaPass.cpp:664` | Implementado — `D3D12Renderer.cpp:7028` (pipeline en `:5620-5647`) | |
| `aaMode = Ssaa` + `ssaaFactor` | Implementado — `Passes/AaPass.cpp:683` | Implementado — `D3D12Renderer.cpp:4260` (escala) + `:7006` (resolve) | El comentario de `:9487-9492` y `D3D12Renderer.h:266-268` dicen que no está: están obsoletos (H47) |
| `aaMode = Msaa` + `msaaSamples` | Implementado — `Renderer.cpp:998` | Implementado — `D3D12Renderer.cpp:4289` + resolve en `:8438` | `maxMsaaSamples()` consulta el device de verdad (`:9463-9479`) |
| `aaMode = Taa` + `taaFeedback` / `taaJitterScale` | Implementado — `Passes/AaPass.cpp:701` | Implementado — `D3D12Renderer.cpp:4659` / `:7023` | Historial doble en `:3490`, jitter en `:2750` |
| `wireframeMode` | Implementado — `Renderer.cpp:1724` | Implementado — `D3D12Renderer.cpp:8018` (skinned `:8121`, cielo apagado `:5476`) | PSOs dedicados en `:2628` y `:2980` |
| `forwardPlusMode = Tiled` | Implementado — `Passes/ForwardPlusPass.cpp:164` / `:474` | Implementado — `D3D12Renderer.cpp:4383` / `:4518` | Mismo `light_cull_tiled.comp`, tile 16 |
| `forwardPlusMode = Clustered` | Implementado — `Passes/ForwardPlusPass.cpp:165` / `:474` | Implementado — `D3D12Renderer.cpp:4384` / `:4518` | 64×64×24 en los dos |
| `forwardPlusLightRadius` | Implementado — `Passes/ForwardPlusPass.cpp:387` | **Implementado (era ausente)** — `D3D12Renderer.cpp:4605` | `setLightRadii` descartaba la lista con un `(void)`. Cerrado, ver H39 |

### 3.2 Resto de features del módulo

| Feature | Vulkan | D3D12 | Nota |
|---|---|---|---|
| PBR | Implementado — `Renderer.cpp:2247` (`pbr.frag.spv`) | Implementado — `D3D12Renderer.cpp:2581` (`pbr.frag.dxil`) | Mismo shader traducido |
| IBL global | Implementado — `Passes/IblPass.cpp:254` | Parcial — `D3D12Renderer.cpp:3833` | La convolución existe, pero el cubemap de origen está fijado en `:4072-4076` (H40) |
| Reflection probes | Implementado — `Passes/ReflectionProbePass.cpp:29` | Implementado — `D3D12Renderer.cpp:5958`, horneado en `:6237` | 8 sondas máx (`:350`), 128 px/cara; `probeCount`/`lastProbeBakeMs` reales (`:9508-9509`) |
| Cascade shadows | Implementado — `Passes/ShadowPass.cpp:364` | Implementado — `D3D12Renderer.cpp:7262` + `:7496` | Matemática duplicada a mano (H3) |
| Skinning por compute | Implementado — `Passes/SkinningPass.cpp:133` | Implementado — `D3D12Renderer.cpp:3195` | Mismos `bone_eval`, `bone_hierarchy` y `skinning.comp` |
| Depth prepass | Implementado — `Passes/DepthPrepassPass.cpp:266` | Implementado — `D3D12Renderer.cpp:5334-5377` | Se graba también sin SSAO cuando lo piden tiled/niebla/SSR/outline (`:5316-5320`) |
| Instancing | Implementado — `Renderer.cpp:1774` | Implementado — `D3D12Renderer.cpp:8066` | Comparten `Renderer/InstanceBatching.h` |
| Frustum culling | Implementado — `Renderer.cpp:1772` (helper `:849`) | Implementado — `D3D12Renderer.cpp:8041-8055` | Comparten `Culling::aabbVisible` |
| Skybox | Implementado — `Renderer.cpp:795` (`initSkybox`) | Parcial — `D3D12Renderer.cpp:5472` dibuja; `initSkybox` sin override | H40 |
| Gizmos | Implementado — `Renderer.cpp:2037` | Implementado — `D3D12Renderer.cpp:8177` (+ `:9005`) | Gateado por `headless` en los dos |
| UI canvas de pantalla | Implementado — `Renderer.cpp:2120` | Implementado — `D3D12Renderer.cpp:6713`, llamada en `:7057` | Atlas y fuentes MSDF propios (`:6424`, `:9393`, `:9433`) |
| UI canvas world-space | Implementado — `Renderer.cpp:1940` | Implementado — `D3D12Renderer.cpp:6614`, llamada en `:8405` | Pipelines con y sin depth en `:5796` |
| Outline de selección | Implementado — `Renderer.cpp:1379` | Implementado — `D3D12Renderer.cpp:7911`, llamada en `:6987` | Sobre el LDR ya tonemapeado, fuera de la captura de sondas |
| Splash screen | Implementado — `Renderer.cpp:285` | **Ausente** — sin override; queda el `return false` de `EditorRenderer.h:66` | El runtime arranca sin fundido bajo D3D12 |
| Subidas asíncronas (`hasPendingUploads`) | Implementado — `Renderer.cpp:4065` | **Ausente** — sin override; queda el `return false` de `EditorRenderer.h:75` | H42 |
| Borrado diferido (`tickDeferredDeletes`) | Implementado — `src/Renderer/DeferredDelete.cpp:1` | Parcial — `D3D12Renderer.cpp:8872` es un no-op declarado | Consecuencia de H32 |
| Métricas GPU por pase (9 getters) | Implementado — `Renderer.h:323-391`, gateadas en `Renderer.cpp:1597` | Implementado — `D3D12Renderer.cpp:9522-9530` (timestamps reales) | `setPerfCaptureEnabled` es no-op en D3D12 (H41) |
| `statDrawCalls` / `statInstances` / `statCulled` | Implementado — `Renderer.cpp:1774`, `:1873`, `:1772` | Implementado — `D3D12Renderer.cpp:9531`, `:9532`, `:9535` | Gateadas solo en Vulkan (H6) |
| `forwardPlusAvgPerCell` / `forwardPlusOverflowCells` | Implementado — `Renderer.h:395-396` | **Implementado (era stub)** — readback en `D3D12Renderer.cpp:4703` y `:4790`, getters en `:9600` | Faltaba solo traer el dato de vuelta. Cerrado, ver H38 |

**Huecos de paridad, en total:** 6 ausentes (`bloomEnabled`, `forwardPlusLightRadius`,
splash, `hasPendingUploads`, estadísticas de Forward+, `setPerfCaptureEnabled`),
3 parciales (IBL, skybox, borrado diferido). Todos son del lado D3D12; ninguno
al revés.

---

## 4. Propuestas

Regla aplicada: toda propuesta es **aditiva** y no rompe audio, física, scripting
Lua, UI, export ni animación. Las que tocan una firma pública o un formato
persistido van marcadas **BREAKING** con su ruta de migración. Los campos nuevos
de `project.json` se leen con `readFloatField(v, clave, valor_actual)`, así que un
proyecto viejo sin la clave conserva exactamente el comportamiento de hoy.

| id | Qué aporta | Dónde se toca | UI del editor que la expone | Riesgo de regresión | Coste |
|---|---|---|---|---|---|
| **P1** | Aviso de recorte de luces: cuando la escena tiene más de `MAX_LIGHTS`, decirlo en vez de recortar en silencio (H1). | `Renderer.cpp:3115` y `D3D12Renderer.cpp:2782` guardan el número real; getter nuevo `sceneLightCount()` en `EditorRenderer.h` con default 0. | Menú **View → sección Forward+** (`EditorUI.cpp:1410`): `ImGui::TextColored` naranja «N luces en escena, 16 iluminan» justo bajo el Combo «Forward+». | Nulo: solo lee y pinta. | S |
| **P2** | Subir el tope de luces del UBO a 64 para que Forward+ sirva de algo (H1). **BREAKING** del layout std140. | `UniformBufferObject.h:6`, los 5 shaders que declaran el bloque, `D3D12Renderer.cpp:91` y el `static_assert` de `:105`. Migración: `MAX_LIGHTS` es interno, no viaja en escena ni en `project.json`; basta recompilar los `.spv`/`.dxil` (no están trackeados, ver el build) y ajustar el offset del `static_assert` en la misma commit. | El aviso de P1 pasa de saltar a 16 a saltar a 64; ningún widget nuevo. | Medio: si el `static_assert` de D3D12 no se ajusta, no compila (fallo ruidoso, que es lo deseable). Si un shader se queda sin recompilar, el UBO se lee desplazado. | M |
| **P3** | Estadísticas de Forward+ reales en D3D12: readback del `fpStatsAllocation` que ya se rellena (H38, H52). | `D3D12Renderer.cpp:9537-9538` + un buffer READBACK leído con dos frames de retraso, igual que los timestamps de `:9522`. | **Menú View → Forward+**, `EditorUI.cpp:1427-1431`, los dos textos que ya existen («Luces/celda» y el aviso naranja de celdas desbordadas) pasan a decir la verdad bajo DX12. | Bajo: solo añade lectura; el compute ya escribe. | S |
| **P4** | Honrar `bloomEnabled()` en D3D12 (H31). | `D3D12Renderer.cpp:6862`: envolver los dispatches del bloom y sumar la contribución solo si el flag está puesto. | **Menú View**, `Checkbox "Bloom"` de `EditorUI.cpp:1096`, que hoy no hace nada bajo DX12. | Bajo, pero cambia la imagen de quien tenía el checkbox apagado y veía bloom igual: es exactamente el bug. | S |
| **P5** · **HECHO** | Selector de skybox por proyecto, hoy inexistente en los dos backends y hardcodeado (`GameExporter.cpp:503`, H40). | `initSkybox` override en `D3D12Renderer` (`:4072-4076` deja de ser fijo); rutas nuevas en `ProjectContext` (`RenderSettings` + `ProjectContext.cpp:117-159`); `Skybox::init` debe destruir lo anterior (H20) para poder recargarse. | **PropertiesPanel**, sección nueva **Environment** con un `drawAssetDropBox` por cara (el mismo widget de las 14 zonas de arrastre ya existentes) y un `Button "Reload skybox"`. | Medio: sin el arreglo de H20 cada cambio filtra un cubemap. El default es `assets/skybox/*.png`, así que un proyecto sin las claves se comporta igual. | M |
| **P6** · **HECHO** | Calidad de sombras ajustable, hoy constantes de compilación (H5). | `RendererState.h` gana `shadowResolution`, `shadowDistance` y `cascadeLambda`; `ShadowPass.h:35`, `ShadowPass.cpp:47` y `D3D12Renderer.cpp:227-231` los leen; `ProjectContext.cpp:117-159` los persiste. | **Menú View → sección Shadows** (nueva, junto a la de Ambient de `EditorUI.cpp:1059`): `Combo "Shadow resolution"` (1024/2048/4096/8192), `SliderFloat "Shadow distance"` 50-2000, `SliderFloat "Cascade lambda"` 0-1. | Medio: cambiar la resolución recrea el texture array, así que el setter tiene que ir por el backend como `setMsaaSamples`, no por `RendererState`. Defaults = los valores de hoy. | M |
| **P7** | Present mode configurable (vsync off), hoy clavado a FIFO (H28). | `Renderer.cpp:938` consulta `vkGetPhysicalDeviceSurfacePresentModesKHR`; en D3D12, el `Present(1, 0)` del `drawFrame` de `:8202-8532`. Virtual nuevo en `EditorRenderer.h` con default no-op. | **Menú View → sección Display** (nueva): `Combo "Present mode"` (Vsync / Mailbox / Immediate), con las opciones no soportadas por el device deshabilitadas y con tooltip del motivo, no ocultas. | Bajo: FIFO sigue siendo el default y siempre está soportado. Desbloquea medir por encima de la tasa de refresco. | M |
| **P8** | Undo/redo en los 39 ajustes de render (H49). | `EditorUI.cpp:1059-1432`: capturar el valor **antes** de dibujar el widget y empujar un `PropertyCommand` en `IsItemDeactivatedAfterEdit`, igual que `PropertiesPanel.cpp:651`. | Los mismos 39 widgets del **menú View**; el cambio es que Ctrl+Z los deshace. | Bajo, con una trampa conocida: `SliderFloat` salta en el frame del click, así que el valor previo hay que leerlo antes de dibujar. Los Combo de `:1319` y `:1416` leen después **a propósito** y deben seguir así. | M |
| **P9** · **HECHO** | Persistir `wireframeMode`, el único campo de `RendererState` que se pierde (H50). | `ProjectContext.h` (`ViewSettings`), `ProjectContext.cpp:117-159` y `:236-289`; `EditorUI.cpp:1581` llama a `saveProjectSettings()`. | **Toolbar**, `Button "Wireframe"` de `EditorUI.cpp:1581`: su estado sobrevive a reabrir el proyecto y llega al juego exportado. | Nulo: clave nueva con default `false` = comportamiento de hoy. | S |
| **P10** | Panel **Rendering** acoplable con los 39 controles, sin quitar el menú View (H58). | Fichero nuevo `src/Editor/RenderingPanel.cpp` que reutiliza los mismos bloques; entrada en el menú **Window**; `panelOpen` nuevo en `ProjectContext.cpp:162-168`. | Panel nuevo **Rendering**, acoplable como Performance; el menú View se queda como acceso rápido. Los siete tiempos de GPU pasan a la tabla con porcentaje que ya usa `PerformancePanel.cpp:197-205` (cierra H57). | Bajo si el menú View se mantiene: nada deja de existir. Ojo con `imgui.ini`, que es la config del usuario. | L |
| **P11** | Liberar de verdad en `removeGameObject` de D3D12 y reciclar huecos con free-list (H32, H43). | `D3D12Renderer.cpp:9112-9123`, `:271-360` y `:8767`; `tickDeferredDeletes` (`:8872`) deja de ser no-op y encola por `fenceValue`, como `DeferredDelete.cpp`. | **PerformancePanel**, fila nueva en la tabla de `:214-216`: `"Slots GPU: N / 512"` y `"Slots skinned: N / 16"`, en naranja al pasar del 90 %. | Alto si se libera antes de que la GPU termine: es exactamente H33/H34. Hacerlo **después** de P16. | M |
| **P12** | Pools de descriptores que crecen en Vulkan, en vez de lanzar (H17, H18). | `SkinningPass.cpp:83` y `Renderer.cpp:2907`/`:2949`: recrear el pool al doble con la GPU en reposo cuando la asignación falla. | **PerformancePanel**, la misma fila de P11 sirve para los dos backends: hoy el objeto 17 no da un contador, da una excepción. | Medio: recrear el pool obliga a rehacer todos los sets del frame; hay que pasar por `waitForGpu`. | M |
| **P13** | Purgar de verdad `m_objects` / `m_skinnedObjects` en Vulkan (H19). | `Renderer.cpp:4131-4147`: hueco libre marcado y reutilizado, o compactación con reindexado; hoy los vectores solo crecen. | **PerformancePanel**, contador de P11: el número deja de subir monótonamente en cada ciclo Play/Stop. | Alto: los índices de render están anotados en cada `GameObject` (`Renderer.cpp:4155-4176`), así que compactar sin reindexar deja a todos apuntando a otra malla. Preferir free-list a compactación. | M |
| **P14** | Camino de subidas asíncrono en D3D12, hoy un stall por recurso (H42). | `D3D12Renderer.cpp:1912-1988`: cola de copia dedicada + `hasPendingUploads()` override; `LoadingModal.cpp` ya sabe consumirlo. | **LoadingModal**, la barra de progreso que hoy salta directa al 100 % bajo DX12 pasa a avanzar de verdad. | Medio: hay que garantizar que nada dibuje un recurso antes de que su fence señale. Vulkan ya resolvió esto (`Renderer.cpp:4065`); copiar ese diseño. | L |
| **P15** · **HECHO** | `setPerfCaptureEnabled` real en D3D12 y gate homogéneo de contadores (H6, H41). | `D3D12Renderer.cpp:9518`, `:8266-8267`, `:8095-8096` y `:8055`. | **PerformancePanel**, `PerformancePanel.cpp:145`: cerrar el panel deja de costar timestamps bajo DX12, igual que ya pasa en Vulkan. | Bajo. | S |
| **P16** | Tratar los fallos de `Signal`, `Close` y `Reset` como fatales en vez de `return` mudo (H33, H34, H45). | `D3D12Renderer.cpp:1755`, `:1871`, `:8256-8259`, `:8510`: `diagLog` + `drainInfoQueue` + marcar el device como perdido, que es el camino que `:1560-1748` ya tiene montado. | **LogPanel**: el fallo aparece como error en vez de como congelación de imagen sin causa. | Bajo, y es prerrequisito de P11: sin esto, liberar recursos tras un `waitForGpu` que no esperó es uso tras liberar. | S |
| **P17** · **HECHO** | Rango completo del SSAA y del MSAA en la UI (H53, H54). | Solo UI: `EditorUI.cpp:1350` y `:1364-1377`. | **Menú View → AA**: el `SliderFloat "SSAA factor"` pasa a `[1.0, 4.0]` (lo que el core acepta, ver `Renderer.h:854`) y los RadioButton de MSAA incluyen `1x`; si `maxMsaaSamples() == 1`, texto explicativo en lugar de un `SameLine()` colgando. | Nulo: no cambia el core. Cumple la regla de que si el core soporta N opciones, la UI ofrece N. | S |
| **P18** · **HECHO** | Mover `ssaaFactor` a `RendererState` (H4). | `RendererState.h` gana el par get/set; `Renderer.h:854` y `D3D12Renderer.h:269` dejan de almacenarlo; `setSsaaFactor` sigue virtual puro en `EditorRenderer.h:233` porque mueve recursos. | **Menú View**, el `SliderFloat "SSAA factor"` de `EditorUI.cpp:1350` sigue igual; desaparece el `static float pendingFactor` de `:1347` (cierra H55). | Bajo: el patrón es idéntico al de `setBloomEnabledFlag` / `setBloomEnabled`, que ya está resuelto así. | S |
| **P19** | Extraer los pases del backend D3D12 a clases con ciclo de vida propio, como en Vulkan (H37). | `D3D12Renderer.cpp:554-1559` (el `struct Impl`) → `src/Renderer/D3D12/Passes/*.cpp`, empezando por los tres más aislados: SSAO (`:5059-5471`), niebla (`:5497-5678` + `:6801-6861`) y motion blur (`:4901-5058`). | **PerformancePanel**: sin cambio visible; la métrica de motion blur (que hoy no lleva `markTimestamp` bajo DX12) pasa a poblarse y su fila deja de salir a 0.00 ms. | Alto por volumen, nulo por diseño si se hace pase a pase con salida byte a byte comparada, que es como se hizo el refactor equivalente en Vulkan. | L |
| **P20** | Helper compartido de carga de shaders, hoy copiado 14 veces (H11). | `src/Renderer/ShaderModule.{h,cpp}` nuevo; los 14 llamantes pasan a usarlo (`ShadowPass.cpp:16-37`, `BloomPass.cpp:16-37`, …). | **LogPanel**: un `.spv` que falta o con `codeSize` no múltiplo de 4 pasa a dar un mensaje con la ruta en vez de un fallo genérico de creación de módulo. | Bajo: es sustitución mecánica con la misma semántica. | S |
| **P21** | Sombras de luz de punto de verdad: cubemap de sombras (6 caras por luz) y proyección en PERSPECTIVA desde la posición de la luz, en vez de la ortográfica en cascada de hoy. Es lo único que hace que la sombra diverja desde la luz y que su tamaño dependa de la distancia. | `Passes/ShadowPass.*` (camino nuevo, no sustituye al de cascadas), el equivalente en `D3D12Renderer.cpp`, y `pbr.frag` con una rama de muestreo por cubemap. | **PropertiesPanel**, sección Light: el aviso naranja «Su sombra es una aproximación» deja de salir, y aparece un `Checkbox "Cast shadows"` por luz. | Alto: es un segundo sistema de sombras conviviendo con el actual, y toca el shader que comparten los dos backends. | L |
| **P22** · **CERRADO** | Que las cascadas dejen de apuntar al origen del mundo. Parche barato mientras no exista P21: la sombra al menos cae bien en una escena no centrada en (0,0,0). Sigue siendo paralela, asi que NO arregla la divergencia de la luz de punto. **Hecho, pero NO como estaba propuesto.** La propuesta decia "al centro de lo que se ve", y asi se implemento primero: el centro del frustum de camara. Verificado en GUI, se descarto — ata la direccion de la luz a la camara, o sea que girar en el sitio gira la sombra, y eso se ve bastante peor que el problema que arregla. El punto de mira definitivo es el centro de la ESCENA (`SceneCenter` en `UniformBufferObject.h`, media de los origenes de objetos y personajes): estable, y la sombra solo se mueve cuando se mueve la luz. Lo calculan los dos backends una vez por frame y lo comparten con la niebla, que necesita la MISMA direccion. De propina, un FOCO pasa a usar su propia `direction` en vez de la aproximacion de la luz de punto (cierra esa parte de H60): tiene cono, o sea direccion de verdad, y antes girar su gizmo no movia su sombra. | `Passes/ShadowPass.*` (parametro `sceneCenter`), `Passes/FogPass.*` (campo del `Context`), `Renderer.*` (miembro `m_sceneCenter`), `D3D12Renderer.cpp` (`computeCascades` y `setLights`) y el helper compartido en `UniformBufferObject.h`. | **PropertiesPanel**, seccion Light: el tooltip del aviso ya no dice «hacia el origen del mundo», y se parte en dos — uno para punto y otro para foco, porque ya no les falta lo mismo. | Medio: cambia donde cae la sombra en escenas que hoy funcionan por estar centradas en el origen. | S |

---

## 5. Prioridad

Orden de ejecución propuesto. Los primeros seis son correcciones; a partir de
P1 son mejoras.

| # | Ítem | Por qué va aquí |
|---|---|---|
| 1 | **P16** (H33, H34, H45) | Corrupción silenciosa por uso tras liberar, y prerrequisito duro de P11 y P13. |
| 2 | ~~**H35**~~ · **CERRADO** | **Mal priorizado**: al ir a arreglarlo resultó no ser alcanzable (ver la fila de H35). Se endureció igual porque cuesta S, pero no era el segundo problema del renderer. |
| 3 | **H36** | Abort al cerrar en Debug sin volcado; el patrón ya está resuelto para los atlas de UI, solo hay que repetirlo. |
| 4 | **P4** (H31) + **H39** | Dos ajustes del editor que bajo DX12 no hacen nada: el usuario cree que apagó el bloom y no lo apagó. Coste S los dos. |
| 5 | **H21, H22, H24, H29** | Cuatro guardas que faltan (bbox vacío, mip 0, `VkResult` ignorados, `surfaceFormats[0]`); coste S cada una y todas producen fallos sin diagnóstico. |
| 6 | **P12** (H17, H18) + **H20** | Topes duros que tumban la carga de escena con 17 personajes o 129 objetos, más la fuga de cubemap al cambiar el cielo. |
| 7 | **P3** (H38, H52) | El panel enseña un diagnóstico falso, que es peor que no enseñarlo; el compute ya escribe el dato. |
| 8 | **P9** (H50) | Coste S, cierra el único campo de `RendererState` que no persiste. |
| 9 | **P17** (H53, H54) | Solo UI, coste S, y cumple la regla de no capar en la interfaz lo que el core acepta. |
| 10 | **P15** (H6, H41) + **P18** (H4, H55) | Dos coherencias baratas: métricas comparables entre backends y una sola casa para `ssaaFactor`. |
| 11 | **P1** (H1) | Hacer visible el recorte de luces antes de gastar en subir el tope: puede que ninguna escena real llegue a 16. |
| 12 | **P8** (H49) | El editor promete Ctrl+Z en todas partes menos aquí; coste M y sin riesgo si se respeta la trampa del `SliderFloat`. |
| 13 | **P11** (H32, H43) + **P13** (H19) | Fugas de VRAM y crecimiento monótono en los dos backends; van después de P16 porque liberar antes de tiempo es peor que filtrar. |
| 14 | **P6** (H5) | La única familia de calidad que no se puede ajustar, y la que más se nota en el rendimiento de un juego exportado. |
| 15 | **P5** (H40, H20) | Cierra el hardcode que `GameExporter.cpp:503` ya documenta como limitación, y con él el IBL global de D3D12. |
| 16 | **P20** (H11) + **H48**, **H26**, **H47** | Limpieza barata: un helper compartido y tres puntos donde la documentación dice lo contrario que el código. |
| 17 | **P7** (H28) | Vsync configurable: desbloquea medir por encima de la tasa de refresco, que es lo que hace falta antes de cualquier trabajo de rendimiento. |
| 18 | **P2** (H1) | Subir `MAX_LIGHTS` solo si P1 demuestra que el recorte ocurre de verdad; es la única propuesta BREAKING. |
| 19 | **P10** (H58, H57) | Ergonomía: afinar efectos exige ver el viewport, y hoy el menú lo tapa. Coste L, sin urgencia. |
| 20 | **P14** (H42) + **P19** (H37) | Los dos trabajos grandes del backend D3D12. Van al final porque son L y porque P19 se hace pase a pase comparando salida byte a byte. |
| 21 | **H7, H8, H9, H10, H12-H16** | Deuda arquitectónica del backend Vulkan. No bloquea nada hoy; se ataca cuando toque el siguiente efecto, extrayendo el pase que ese efecto necesite. |
