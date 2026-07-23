# Splash screen del juego exportado — Design

**Fecha:** 2026-07-23
**Estado:** Diseño aprobado, pendiente de plan de implementación

## Problema

El juego exportado (`DonTopoRuntime.exe`) muestra la ventana en negro durante
toda la carga y solo pinta el primer frame cuando ya está todo listo. Medido en
Release con un solo personaje skinned:

| fase | tiempo |
| --- | --- |
| `window.init` (ventana ya visible, negra) | 20 ms |
| `renderer.init` (Vulkan, shaders, pipelines) | ~930 ms |
| `initSkybox` (6 texturas cubemap) | 44 ms |
| `addSkinnedMesh` (1 personaje) | ~1024 ms |
| `scriptManager.init` | 36 ms |
| primer frame | 27 ms |
| **total de ventana negra** | **≈ 2.0 s** |

`addSkinnedMesh` escala con el contenido: una escena real será más. Son ~2 s de
ventana negra que conviene tapar con la imagen del motor.

## Restricción técnica que decide el enfoque

No se puede dibujar con Vulkan hasta que Vulkan está inicializado, y esa init es
parte de lo que se quiere tapar. Desglose de `renderer.init` (~930 ms):

| sub-fase | ms | ¿diferible? |
| --- | --- | --- |
| `gpu.init` (device Vulkan) | 221 | **No** |
| `createSwapChain` | 298 | **No** |
| pipelines PBR + shadow + compute | ~110 | Sí |
| resto (render pass, framebuffers, descriptors, sync) | el resto | mayoría necesaria |

Device + swapchain (~520 ms) son obligatorios antes de poder presentar el primer
píxel. **El negro inicial mínimo con un splash hecho en Vulkan es ~600 ms**
(device + swapchain + el pipeline del splash). Diferir los pipelines de escena
ahorra ~150-200 ms; el splash tapa el resto (skybox + meshes + scripts, ~1.4 s,
que es lo que más crece con escenas grandes).

## Enfoque elegido: splash Vulkan, fase 1 (sin hilos)

Se descartó un splash nativo Win32 (taparía el 100% incluido el arranque de
Vulkan) porque el proyecto irá a multithread en el futuro y el splash bueno
—worker cargando mientras el hilo principal anima el logo a 60 fps— es el de
Vulkan. El nativo sería un callejón que se tira. Se acepta el negro inicial de
~600 ms a cambio de una base reutilizable.

Fase 1 (esta feature) monta el pipeline del splash y el bucle de presentación
single-thread, intercalando frames de splash entre las unidades de carga. Fase 2
(futura, fuera de alcance) sustituye el intercalado por un worker thread sin
tocar el pipeline ni el bucle — solo cambia la fuente del progreso.

Resultado esperado: ventana negra de **2.0 s → ~0.6 s**, con el logo tapando el
resto de la carga.

## Arquitectura

### `Renderer::init` se parte en dos fases

Hoy `Renderer::init(window, meshes)` es monolítico. Se separa en:

- `initPresentation(Window&)` — lo mínimo para presentar: device, swapchain,
  image views, depth, render pass del swapchain, framebuffers, command buffers,
  sync. Bloque no-diferible (~520 ms).
- `initSceneResources(const std::vector<Mesh>&)` — lo diferible: pipelines
  PBR/shadow/compute, offscreen images, descriptor sets, subida de mallas
  estáticas.
- `init(window, meshes)` sigue existiendo y llama a las dos en orden. **El editor
  (Sandbox) llama a `init()` y no cambia en nada.**

### Nueva clase `SplashScreen`

Mismo molde que `Skybox` (clase aislada en `engine/src/Renderer/`, sin
dependencia de Scene/ImGui/Editor):

- `bool init(GpuDevice&, VkRenderPass, VkFormat, const std::string& logoPath)` —
  crea pipeline, sube la textura del logo (`GpuResources::createTextureImage`, ya
  existe), descriptor set. Devuelve `false` si el PNG no carga; el runtime se
  salta el splash entero (nunca bloquea el arranque por un logo ausente).
- `recordDraw(VkCommandBuffer, float alpha)` — bind pipeline + push constant
  `alpha` + `vkCmdDraw(3, 1, 0, 0)` (triángulo fullscreen, sin vertex buffer,
  igual que el Skybox).
- `shutdown(VkDevice)` — libera pipeline, textura, layouts.

Shader nuevo: vert genera el triángulo fullscreen; frag muestrea la textura del
logo con **letterbox** (mantiene el aspect ratio del logo sobre el aspecto de la
ventana) y multiplica por `alpha` para el fade.

### `Renderer` — API añadida

- `initPresentation`, `initSceneResources` públicos (arriba).
- `beginSplash(const std::string& logoPath)` → `m_splash.init(...)` sobre el
  render pass del swapchain ya creado por `initPresentation`.
- `drawSplashFrame(float alpha)` → mini-drawFrame: adquiere imagen del swapchain,
  graba un render pass mínimo (clear + `m_splash.recordDraw(alpha)`), submit,
  present. No toca el pipeline de escena.
- `m_splash` como miembro por valor (como `m_skybox`). El editor nunca llama a
  `beginSplash`, así que su `SplashScreen` queda sin `init` y sin coste.

### `runtime/main.cpp` — reorden

Sustituye la única llamada `renderer.init(window, meshes)` por:

```
1. window.init                                    [negro, 20ms]
2. renderer.initPresentation(window)              [negro, ~520ms]
3. renderer.beginSplash(logo); drawSplashFrame(0) [LOGO visible, ~600ms]
4. carga pesada, intercalando drawSplashFrame(alpha):
     - renderer.initSceneResources(meshes)
     - initSkybox
     - por cada malla skinned: addSkinnedMesh + drawSplashFrame
     - scriptManager.init
5. respetar tiempo minimo de splash (bucle de drawSplashFrame)
6. cross-fade a la escena (escena + logo con alpha decreciente)
7. game loop normal
```

La política de fade/mínimo/intercalado vive en el runtime (no en el Renderer),
en un helper local `SplashDriver` que lleva el reloj y calcula el alpha según
fase. `SplashDriver` es una función/estado puro, sin GPU, testeable aparte.

## Empaquetado del logo

La escena no referencia el logo, así que el exportador lo copia **siempre**,
como el skybox y los shaders:

- Fuente: `assets/MainEngineLogo.png` (ya existe en el repo).
- El exportador lo copia al paquete como `splash.png` junto al `.exe`.
- El runtime carga `splash.png` de su directorio de trabajo. Si falta → sin
  splash, arranque directo, nunca aborta.
- En dev (runtime sin exportar), fallback a `assets/MainEngineLogo.png` para
  poder verlo sin exportar.

## Fade y tiempos

Constantes en el runtime, fáciles de tocar:

- **fade-in** ~300 ms: alpha 0→1 sobre el color de fondo, desde que el splash
  puede pintar (~600 ms).
- **hold**: mientras dura la carga real (intercalando frames).
- **mínimo total** ~1.5 s: si la carga acaba antes, el runtime sigue dibujando
  frames de splash hasta cumplirlo, para que "unos segundos" se cumpla y el fade
  no pase desapercibido.
- **cross-fade** ~300 ms: escena renderizada + logo con alpha 1→0 encima.
  Fallback si se complica el compuesto: fade del logo a fondo + corte a escena.

El splash es independiente de la escena: si la escena falla al cargar, el runtime
ya sale con error antes (sin cambios).

## Testing

Automatizable:

- `SplashScreen::init` con un PNG válido → `true`; con ruta inexistente →
  `false` sin crashear (garantía de "logo ausente no bloquea").
- Exportador: el paquete contiene `splash.png` (extiende `exporter_tests.cpp`).
- `SplashDriver` como función pura: fade-in sube 0→1, hold se mantiene en 1,
  cross-fade baja 1→0, y el mínimo de tiempo se respeta. Sin GPU.

Solo verificación manual (usuario):

- Que el logo se vea, el fade sea suave, el letterbox respete el aspect ratio, y
  no haya flash negro en el cross-fade.
- Fixtures: una escena con un personaje skinned (carga ~1 s, tiempo de sobra
  para ver el splash) y otra vacía (para ver que se respeta el mínimo de 1.5 s).

## Fuera de alcance

- **Fase 2 multithread**: worker cargando mientras el splash anima a 60 fps.
  Necesita resolver `graphicsQueue` (solo hay una cola gráfica, sin transfer
  queue dedicada; `VkQueue` no es thread-safe y todo upload hace
  `vkQueueSubmit + vkQueueWaitIdle`). Continuación natural, se diseña con el
  multithread general.
- **Editor**: sin splash. Solo el runtime exportado.
- **Reducir los ~600 ms de arranque de Vulkan** (device + swapchain).
  `createSwapChain` a 298 ms es sospechosamente alto y quizá optimizable, pero
  es otra tarea.
- **Logo configurable por proyecto** desde el editor. Por ahora fijo
  (`MainEngineLogo.png`). Se puede añadir después sin rehacer nada.

## Riesgos

- **Cross-fade (paso 6)**: componer el logo con alpha↓ sobre la escena ya
  renderizada es la parte más acoplada. Fallback definido: fade del logo a fondo
  + corte.
- **Reordenar `renderer.init`**: partir un init monolítico puede destapar
  dependencias de orden implícitas entre sub-fases. Mitigación: `init()` sigue
  llamando a las dos fases en el mismo orden que hoy, así el editor valida que la
  secuencia completa no cambió; solo el runtime intercala.
